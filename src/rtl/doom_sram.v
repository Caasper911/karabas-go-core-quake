`timescale 1ns / 1ps
`default_nettype none
/*
 doom_sram.v

 The SRAM of the board (two 2M x 8 chips = one 2M x 16, 10 ns, asynchronous; MD[7:0] / MWR_N[0] /
 MRD_N[0] the low byte) at the CPU clock (clk, 75 MHz), after archie_sram.v (core-archie), two ports:

 - CPU, a wishbone slave as the VexRiscv buses use it: a line fill (CTI 010, 8 words from the first word
   of the line) or a single read, each word acked when its two halfwords are in; a write of one word
   with the byte selects, per halfword lane pair (the lanes of a halfword in one write), acked after it.
 - video: vid_req (a pulse) asks vid_len halfwords from halfword address vid_adr; they come out as
   vid_we / vid_wa (0..vid_len-1) / vid_wd. Served between CPU transactions, VID_CHUNK halfwords at a
   time, so that the CPU waits a little only; vid_busy until done.
 - drawer (Ф1в, doom_draw): drw_req asks a write of the lanes drw_sel of halfword drw_adr, acked by
   drw_ack; after the CPU (which waits while the drawer is busy: doom_soc), before nothing else.
   Quake Ф3 (quake_span): drw_we 0 = a read of halfword drw_adr instead, drw_rdat with drw_ack (as a DMA
   read: the address out two clocks, the data a clock later).
 - DMA: dma_req (a pulse) copies dma_len halfwords from dma_src to dma_dst (the frame drawn by the CPU
   into a buffer of the video, docs/PLAN.md Ф1): in runs of DMA_RUN halfwords, the video first between
   runs; the CPU waits until the copy is done (the frame must not change under it). dma_busy until done.
   A halfword: read in two clocks, written in three: ~2.1 ms for a 320x200 frame at 75 MHz.

 Read: a halfword in two clocks (26.7 ns at 75 MHz): MA from an IOB flip-flop, held two clocks, MD into
 an IOB flip-flop at the end of the second (clock to pad + 10 ns + pad to flip-flop: TS_SRAM_OUT /
 TS_SRAM_IN of the ucf). Write: a halfword in three clocks: MA and MD out with /OE off, /WE of the lanes
 low, /WE high (address and data held a clock around /WE).
*/
module doom_sram #(
	parameter VID_CHUNK = 6'd32,
	parameter DMA_RUN = 7'd64
) (
	input wire clk,

	// CPU (byte address of the 4 MB)
	input wire wb_cyc,
	input wire wb_stb,
	input wire wb_we,
	input wire [3:0] wb_sel,
	input wire [21:2] wb_adr,
	input wire [2:0] wb_cti,
	input wire [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o = 32'd0,
	output reg wb_ack = 1'b0,

	// video
	input wire vid_req,
	input wire [20:0] vid_adr,
	input wire [8:0] vid_len,
	output reg vid_we = 1'b0,
	output reg [8:0] vid_wa = 9'd0,
	output reg [15:0] vid_wd = 16'd0,
	output wire vid_busy,

	// drawer: a write of the lanes of a halfword
	input wire drw_req,
	input wire [20:0] drw_adr,
	input wire [1:0] drw_sel,
	input wire [15:0] drw_dat,
	input wire drw_we,                    // Quake Ф3: 0 = read (drw_rdat)
	output reg [15:0] drw_rdat = 16'd0,
	output reg drw_ack = 1'b0,

	// DMA (halfword addresses)
	input wire dma_req,
	input wire [20:0] dma_src,
	input wire [20:0] dma_dst,
	input wire [15:0] dma_len,
	output wire dma_busy,

	// pins (the top makes the tristate of MD)
	output wire [20:0] MA,
	input wire [15:0] MD_I,
	output wire [15:0] MD_O,
	output wire [15:0] MD_OE_N,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N
);

(* IOB = "TRUE" *) reg [20:0] ma_r = 21'd0;
(* IOB = "TRUE" *) reg [1:0] mwr_r = 2'b11;
(* IOB = "TRUE" *) reg [1:0] mrd_r = 2'b00;
(* IOB = "TRUE" *) reg [15:0] md_out = 16'd0;
(* IOB = "TRUE", equivalent_register_removal = "no" *) reg [15:0] md_oe_n = 16'hFFFF;
(* IOB = "TRUE" *) reg [15:0] md_in = 16'd0;

assign MA = ma_r;
assign MWR_N = mwr_r;
assign MRD_N = mrd_r;
assign MD_O = md_out;
assign MD_OE_N = md_oe_n;

localparam [3:0] S_IDLE = 4'd0, S_RD = 4'd1, S_W1 = 4'd2, S_W2 = 4'd3, S_W3 = 4'd4, S_VID = 4'd5,
	S_DR = 4'd6, S_DW1 = 4'd7, S_DW2 = 4'd8, S_DW3 = 4'd9, S_QR = 4'd10;

reg [3:0] st = S_IDLE;
wire wb_req = wb_cyc & wb_stb & ~wb_ack;
reg [21:2] q_adr = 20'd0;
reg [31:0] q_dat = 32'd0;
reg [3:0] q_sel = 4'd0;
reg q_hi = 1'b0;                  // write: the halfword being written
reg q_drw = 1'b0;                 // write: the drawer's (acked on drw_ack)
reg [4:0] rd_n = 5'd0;            // read: halfwords done
reg [4:0] rd_last = 5'd0;         // read: halfwords of the request (2 or 16)
reg ph = 1'b0;                    // read: the second clock of a halfword
reg [15:0] lo_half = 16'd0;

reg vid_pend = 1'b0;
reg [20:0] v_adr = 21'd0;
reg [8:0] v_left = 9'd0;
reg [8:0] v_n = 9'd0;
reg [5:0] v_run = 6'd0;
reg v_ph = 1'b0;
reg cpu_last = 1'b0;              // the last transaction was the CPU's: video next if pending
assign vid_busy = vid_pend;

reg dma_pend = 1'b0;
reg [20:0] d_src = 21'd0, d_dst = 21'd0;
reg [15:0] d_left = 16'd0;
reg [6:0] d_run = 7'd0;
reg [1:0] d_ph = 2'd0;
assign dma_busy = dma_pend;

always @(posedge clk) begin
	wb_ack <= 1'b0;
	drw_ack <= 1'b0;
	vid_we <= 1'b0;
	md_in <= MD_I;
	if (dma_req && !dma_pend && dma_len != 16'd0) begin
		dma_pend <= 1'b1;
		d_src <= dma_src;
		d_dst <= dma_dst;
		d_left <= dma_len;
	end
	if (vid_req && !vid_pend && vid_len != 9'd0) begin
		vid_pend <= 1'b1;
		v_adr <= vid_adr;
		v_left <= vid_len;
		v_n <= 9'd0;
	end

	case (st)
	S_IDLE: begin
		md_oe_n <= 16'hFFFF;
		mrd_r <= 2'b00;
		if (vid_pend && (cpu_last || !wb_req || dma_pend)) begin
			cpu_last <= 1'b0;
			ma_r <= v_adr;
			v_ph <= 1'b0;
			v_run <= VID_CHUNK;
			st <= S_VID;
		end else if (dma_pend) begin
			cpu_last <= 1'b1;
			ma_r <= d_src;
			d_ph <= 2'd0;
			d_run <= DMA_RUN;
			st <= S_DR;
		end else if (wb_req) begin
			cpu_last <= 1'b1;
			q_drw <= 1'b0;
			q_adr <= wb_adr;
			q_dat <= wb_dat_i;
			q_sel <= wb_sel;
			if (wb_we) begin
				if (wb_sel[1:0] != 2'b00) begin
					q_hi <= 1'b0;
					ma_r <= { wb_adr[21:2], 1'b0 };
					md_out <= wb_dat_i[15:0];
				end else begin
					q_hi <= 1'b1;
					ma_r <= { wb_adr[21:2], 1'b1 };
					md_out <= wb_dat_i[31:16];
				end
				md_oe_n <= 16'h0000;
				mrd_r <= 2'b11;
				st <= S_W1;
			end else begin
				ma_r <= { wb_adr[21:2], 1'b0 };
				rd_n <= 5'd0;
				ph <= 1'b0;
				rd_last <= (wb_cti == 3'b010) ? 5'd16 : 5'd2;
				st <= S_RD;
			end
		end else if (drw_req && !drw_ack && !drw_we) begin
			// Quake Ф3: the drawer reads a halfword (S_QR: as S_DR)
			cpu_last <= 1'b1;
			ma_r <= drw_adr;
			d_ph <= 2'd0;
			st <= S_QR;
		end else if (drw_req && !drw_ack) begin
			// the drawer: one halfword (S_W1..S_W3 with q_hi 0 and lanes 3..2 empty)
			cpu_last <= 1'b1;
			q_drw <= 1'b1;
			q_hi <= 1'b0;
			q_sel <= { 2'b00, drw_sel };
			ma_r <= drw_adr;
			md_out <= drw_dat;
			md_oe_n <= 16'h0000;
			mrd_r <= 2'b11;
			st <= S_W1;
		end
	end

	// a halfword every two clocks: the address of halfword rd_n is out (ph 0, 1); md_in takes it at
	// the end of ph 1 and is used a clock later (ph 0 of the next one, or here in S_IDLE's clock)
	S_RD: begin
		ph <= ~ph;
		if (ph) begin
			rd_n <= rd_n + 5'd1;
			if (rd_n != rd_last - 5'd1)
				ma_r <= { q_adr[21:5], q_adr[4:2] + rd_n[3:1] + { 2'b00, rd_n[0] }, ~rd_n[0] };
		end
		// md_in now holds halfword rd_n - 1 (taken at the end of its ph 1)
		if (!ph && rd_n != 5'd0) begin
			if (rd_n[0]) lo_half <= md_in;
			else begin
				wb_dat_o <= { md_in, lo_half };
				wb_ack <= 1'b1;
			end
			if (rd_n == rd_last) st <= S_IDLE;
		end
	end

	S_W1: begin
		mwr_r <= q_hi ? ~q_sel[3:2] : ~q_sel[1:0];
		st <= S_W2;
	end
	S_W2: begin
		mwr_r <= 2'b11;
		st <= S_W3;
	end
	S_W3: begin
		// the address and data held a clock after /WE; then the high half if it has lanes
		if (!q_hi && q_sel[3:2] != 2'b00) begin
			q_hi <= 1'b1;
			ma_r <= { q_adr[21:2], 1'b1 };
			md_out <= q_dat[31:16];
			st <= S_W1;
		end else begin
			if (q_drw) drw_ack <= 1'b1; else wb_ack <= 1'b1;
			md_oe_n <= 16'hFFFF;
			mrd_r <= 2'b00;
			st <= S_IDLE;
		end
	end

	// video: a halfword every two clocks, as S_RD
	S_VID: begin
		v_ph <= ~v_ph;
		if (v_ph) begin
			v_adr <= v_adr + 21'd1;
			v_left <= v_left - 9'd1;
			v_run <= v_run - 6'd1;
			ma_r <= v_adr + 21'd1;
		end
		if (!v_ph && v_run != VID_CHUNK) begin
			vid_we <= 1'b1;
			vid_wa <= v_n;
			vid_wd <= md_in;
			v_n <= v_n + 9'd1;
			if (v_left == 9'd0) vid_pend <= 1'b0;
			if (v_left == 9'd0 || v_run == 6'd0) st <= S_IDLE;
		end
	end
	// DMA: the halfword at d_src (address out for two clocks, md_in at the end of the second)
	S_DR: begin
		d_ph <= d_ph + 2'd1;
		if (d_ph == 2'd2) begin
			ma_r <= d_dst;
			md_out <= md_in;
			md_oe_n <= 16'h0000;
			mrd_r <= 2'b11;
			st <= S_DW1;
		end
	end
	S_DW1: begin
		mwr_r <= 2'b00;
		st <= S_DW2;
	end
	S_DW2: begin
		mwr_r <= 2'b11;
		st <= S_DW3;
	end
	S_DW3: begin
		md_oe_n <= 16'hFFFF;
		mrd_r <= 2'b00;
		d_src <= d_src + 21'd1;
		d_dst <= d_dst + 21'd1;
		d_left <= d_left - 16'd1;
		d_run <= d_run - 7'd1;
		if (d_left == 16'd1) begin
			dma_pend <= 1'b0;
			st <= S_IDLE;
		end else if (d_run == 7'd1 || vid_pend)
			st <= S_IDLE;
		else begin
			ma_r <= d_src + 21'd1;
			d_ph <= 2'd0;
			st <= S_DR;
		end
	end
	// Quake Ф3: the drawer's read: md_in holds the halfword at d_ph 2 (as S_DR)
	S_QR: begin
		d_ph <= d_ph + 2'd1;
		if (d_ph == 2'd2) begin
			drw_rdat <= md_in;
			drw_ack <= 1'b1;
			st <= S_IDLE;
		end
	end
	default: st <= S_IDLE;
	endcase
end

endmodule
