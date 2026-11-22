`timescale 1ns / 1ps
`default_nettype none
/*
 doom_soc.v

 The Doom machine without the board glue: VexRiscv (rv32im, I$ 32K, D$ 32K, wishbone), the SDRAM
 (doom_sdram), the SRAM (doom_sram), the MMIO registers, the loader. One clock (clk, 75 MHz; Mini G 875 / 11 = 79.545 MHz: CLK_NUM / CLK_DEN).

 Memory map of the CPU (docs/PLAN.md, src/fw/mmio.h):
   0x00000000 SRAM 4 MB (the frame 0x3f0000)       0x10000000 SDRAM 32 MB (program 0, WAD 0xc00000)
   0x90000000 SRAM uncached (Ф1в: the frame read behind the drawer)
   0x80000000 MMIO (uncached: VexRiscv IO range = bit 31), word registers:
     00 W  UART: a byte of the console (kept as dbg_uart for the debug output)
     04 W  FRAME: a frame is drawn: copied by DMA (doom_sram) into the display buffer not shown
           (disp_front toggles when done; the video flips at its next frame, disp_shown); the CPU
           waits for the SRAM meanwhile (counted in frame_n)
     08 W  EXIT: the code (kept as dbg_exit, bit 8 set: I_Error / exit ran)
     0c R  CYCLES: clocks since reset (low 32)        1c R  MS: milliseconds since reset
     20 R  WADSIZE: bytes of the WAD (0: none yet)
     28 R  KBD0: kg_mcu2 KBD[31:0] = { key 3, key 2, key 1, modifiers } (the USB boot report)
     2c R  KBD1: { 0, key 6, key 5, key 4 }            30 R  KBDN: reports seen (a counter)
     34 R  MOUSE: see below, reading clears the sums
     38 R  JOY: { joy_r, joy_l }
     40 R  US: microseconds since reset (low 32)
     44 W  TCMP: the timer interrupt (VexRiscv timerInterrupt, level) is on while TEN and US - TCMP >= 0
     48 W  TEN: bit 0
     50 R  SW: the OSD switches (kg_mcu2 SW: FPS 7..0, Music 15..8, Controls 23..16, Mouse move 31..24)
     60..70, 2000..3ffc: doom_audio (OPL3, MIDI, the SFX ring)
     80..90, 1000..17fc: doom_sd (the save file, its sector buffer)
     c0..dc: doom_draw (Ф1в, the drawer of the view: columns and spans into the work frame)
     400..7fc W  PAL: palette entry ((a - 400) / 4) = 0x00RRGGBB
   MOUSE: bits 31..24 buttons, 23..12 dy sum, 11..0 dx sum (signed 12 bit, saturated; reading clears).
 Instruction fetch: the SDRAM only (the program). The I and D buses and the drawer share the SDRAM (D
 first, then I, then the drawer), the owner keeps it to the last ack of its transaction (CTI 010 = more
 beats). The drawer (doom_draw) writes the work frame through its own SRAM port; while it is busy the CPU
 accesses to the SRAM wait (the order of the frame: the CPU draws over it later), and so does the frame
 copy of a FRAME write.

 Loader (kg_mcu2 LD_*, while the CPU is in reset): kind 0 (the ROMs of the .kg) to the SDRAM from 0,
 kind 2 slot 0 (the WAD item) to SDRAM 0xc00000; a byte per SDRAM write (byte select), ld_ready low
 while one is pending. WADSIZE = the bytes of the last WAD load (its highest address + 1), set at its
 end; a new WAD load clears it first. The CPU is held in reset while anything loads (cpu_hold) and
 until the SDRAM is up.
*/
module doom_soc #(
	parameter SDRAM_INIT_CLOCKS = 15'd16000,
	// the clock in MHz = CLK_NUM / CLK_DEN (75 / 1; 875 / 11 = 79.545 MHz): US and MS count exactly
	parameter [9:0] CLK_NUM = 10'd75,
	parameter [9:0] CLK_DEN = 10'd1,
	parameter [31:0] RESET_VECTOR = 32'h10000000,
	// Quake (karabas): 0 = no doom_draw (its registers read 0, STAT without the id: the firmware draws itself)
	parameter WITH_DRAW = 1,
	// Quake Ф3: 1 = quake_span (MMIO 0x100..0x13c) on the drawer port of the SRAM (needs WITH_DRAW 0); the CPU
	// waits for the SRAM at >= 0x3b0000 (z-buffer and frames) and the frame copy waits while it is busy
	parameter WITH_QSPAN = 0
) (
	input wire clk,
	input wire reset,                     // the whole machine (PLL, the Reset item)

	// loader (kg_mcu2)
	input wire [1:0] ld_kind,
	input wire [7:0] ld_slot,
	input wire ld_start,
	input wire ld_active,
	input wire ld_end,
	input wire [31:0] ld_addr,
	input wire [7:0] ld_data,
	input wire ld_wr,
	output wire ld_ready,

	// HID (kg_mcu2)
	input wire [55:0] kbd,
	input wire kbd_wr,
	input wire [7:0] ms_x,
	input wire [7:0] ms_y,
	input wire [7:0] ms_b,
	input wire ms_wr,
	input wire [15:0] joy_l,
	input wire [15:0] joy_r,
	input wire [31:0] sw,

	// doom_audio registers
	output wire aud_we,
	output wire [13:0] aud_off,
	output wire [31:0] aud_wd,
	input wire [31:0] aud_rd,

	// video: the SRAM read port and the palette writes
	input wire vid_req,
	input wire [20:0] vid_adr,
	input wire [8:0] vid_len,
	output wire vid_we,
	output wire [8:0] vid_wa,
	output wire [15:0] vid_wd,
	output wire vid_busy,
	output reg pal_we = 1'b0,
	output reg [7:0] pal_wa = 8'd0,
	output reg [23:0] pal_wd = 24'd0,
	output reg [15:0] frame_n = 16'd0,
	output reg disp_front = 1'b0,         // the display buffer with the last whole frame
	input wire disp_shown,                // the one the video shows (synchronised to clk)

	// debug
	output reg [7:0] dbg_uart = 8'd0,
	output reg [8:0] dbg_exit = 9'd0,
	output wire cpu_run,
	output wire [7:0] dbg_state,          // { copy_pend, copy_run, dma_busy, disp_front, disp_shown, timer_irq, ten, 0 }
	output reg [7:0] dbg_tcmp_n = 8'd0,   // TCMP writes (one per timer interrupt)

	// SDRAM pins (the top makes the tristate)
	output wire [12:0] sdr_a,
	output wire [1:0] sdr_ba,
	input wire [15:0] sdr_dq_i,
	output wire [15:0] sdr_dq_o,
	output wire [15:0] sdr_dq_oe_n,
	output wire [1:0] sdr_dqm,
	output wire sdr_ras_n,
	output wire sdr_cas_n,
	output wire sdr_we_n,

	// SRAM pins
	output wire [20:0] MA,
	input wire [15:0] MD_I,
	output wire [15:0] MD_O,
	output wire [15:0] MD_OE_N,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N
);

// ------------------------------------------------------------------ reset / loader

wire sdram_ready;
reg cpu_reset = 1'b1;
assign cpu_run = ~cpu_reset;
always @(posedge clk)
	cpu_reset <= reset | ld_active | ~sdram_ready;

reg [31:0] wad_size = 32'd0;
reg [31:0] wad_top = 32'd0;
reg ld_pend = 1'b0;
reg [24:0] ld_a = 25'd0;
reg [7:0] ld_d = 8'd0;
wire ld_is_wad = ld_kind == 2'd2 && ld_slot == 8'd0;
assign ld_ready = ~ld_pend;

// ------------------------------------------------------------------ CPU

wire i_cyc, i_stb, d_cyc, d_stb, d_we;
wire [29:0] i_adr, d_adr;
wire [31:0] d_dat_o;
wire [3:0] d_sel;
wire [2:0] i_cti, d_cti;
reg [31:0] i_dat_i, d_dat_i;
reg i_ack, d_ack;

VexRiscv cpu (
	.externalResetVector(RESET_VECTOR),
	.timerInterrupt(timer_irq),
	.softwareInterrupt(1'b0),
	.externalInterruptArray(32'd0),
	.iBusWishbone_CYC(i_cyc),
	.iBusWishbone_STB(i_stb),
	.iBusWishbone_ACK(i_ack),
	.iBusWishbone_WE(),
	.iBusWishbone_ADR(i_adr),
	.iBusWishbone_DAT_MISO(i_dat_i),
	.iBusWishbone_DAT_MOSI(),
	.iBusWishbone_SEL(),
	.iBusWishbone_ERR(1'b0),
	.iBusWishbone_CTI(i_cti),
	.iBusWishbone_BTE(),
	.dBusWishbone_CYC(d_cyc),
	.dBusWishbone_STB(d_stb),
	.dBusWishbone_ACK(d_ack),
	.dBusWishbone_WE(d_we),
	.dBusWishbone_ADR(d_adr),
	.dBusWishbone_DAT_MISO(d_dat_i),
	.dBusWishbone_DAT_MOSI(d_dat_o),
	.dBusWishbone_SEL(d_sel),
	.dBusWishbone_ERR(1'b0),
	.dBusWishbone_CTI(d_cti),
	.dBusWishbone_BTE(),
	.clk(clk),
	.reset(cpu_reset)
);

reg timer_irq = 1'b0;
wire d_req = d_cyc & d_stb;
wire copy_pend_w, copy_run_w, dma_busy_w;
wire i_req = i_cyc & i_stb;
wire d_io = d_adr[29] & ~d_adr[26];        // 0x80000000
wire d_uc = d_adr[29] & d_adr[26];          // 0x90000000: the SRAM uncached
wire d_sd = ~d_adr[29] & d_adr[26];         // 0x10000000
wire d_sr = (~d_adr[29] & ~d_adr[26]) | d_uc;

// ------------------------------------------------------------------ SDRAM and its owner

localparam [2:0] O_NONE = 3'd0, O_D = 3'd1, O_I = 3'd2, O_LD = 3'd3, O_DR = 3'd4;
reg [2:0] own = O_NONE;
// the drawer
wire drw_sd_cyc, drw_busy, drw_push_wait, drw_sr_req, drw_sr_ack;
wire [24:2] drw_sd_adr;
wire [2:0] drw_sd_cti;
wire [20:0] drw_sr_adr;
wire [1:0] drw_sr_sel;
wire [15:0] drw_sr_dat;
wire drw_sr_we;                           // Quake Ф3: 0 = a read (quake_span texels)
wire [15:0] drw_sr_rdat;
wire qsp_busy, qsp_push_wait;
wire sd_ack;
wire [31:0] sd_dat;
reg sd_cyc, sd_we;
reg [3:0] sd_sel;
reg [24:2] sd_adr;
reg [2:0] sd_cti;
reg [31:0] sd_wdat;

always @(*) begin
	sd_cyc = 1'b0; sd_we = 1'b0; sd_sel = 4'hF; sd_adr = 23'd0; sd_cti = 3'd0; sd_wdat = 32'd0;
	case (own)
	O_D: begin sd_cyc = d_req; sd_we = d_we; sd_sel = d_sel; sd_adr = d_adr[22:0]; sd_cti = d_cti; sd_wdat = d_dat_o; end
	O_I: begin sd_cyc = i_req; sd_we = 1'b0; sd_sel = 4'hF; sd_adr = i_adr[22:0]; sd_cti = i_cti; end
	O_LD: begin sd_cyc = ld_pend; sd_we = 1'b1; sd_sel = 4'b0001 << ld_a[1:0]; sd_adr = ld_a[24:2]; sd_cti = 3'd0; sd_wdat = {4{ld_d}}; end
	O_DR: begin sd_cyc = drw_sd_cyc; sd_we = 1'b0; sd_sel = 4'hF; sd_adr = drw_sd_adr; sd_cti = drw_sd_cti; end
	default: ;
	endcase
end

// the owner: kept to the last ack of a transaction (an ack with CTI not 010)
always @(posedge clk) begin
	if (cpu_reset) begin
		own <= ld_pend ? O_LD : O_NONE;
		if (own == O_LD && sd_ack) own <= O_NONE;
	end else if (own == O_NONE) begin
		if (d_req && d_sd) own <= O_D;
		else if (i_req) own <= O_I;
		else if (drw_sd_cyc) own <= O_DR;
	end else if (sd_ack && sd_cti != 3'b010)
		own <= O_NONE;
end

doom_sdram #(.INIT_CLOCKS(SDRAM_INIT_CLOCKS)) sdram (
	.clk(clk),
	.reset(reset),
	.ready(sdram_ready),
	.wb_cyc(sd_cyc), .wb_stb(sd_cyc), .wb_we(sd_we), .wb_sel(sd_sel), .wb_adr(sd_adr), .wb_cti(sd_cti),
	.wb_dat_i(sd_wdat), .wb_dat_o(sd_dat), .wb_ack(sd_ack),
	.sdr_a(sdr_a), .sdr_ba(sdr_ba), .sdr_dq_i(sdr_dq_i), .sdr_dq_o(sdr_dq_o), .sdr_dq_oe_n(sdr_dq_oe_n),
	.sdr_dqm(sdr_dqm), .sdr_ras_n(sdr_ras_n), .sdr_cas_n(sdr_cas_n), .sdr_we_n(sdr_we_n)
);

// ------------------------------------------------------------------ SRAM

wire sr_ack;
wire [31:0] sr_dat;
// the frame copy: asked by the FRAME write, started when the video shows the last copy (the target
// is the other buffer); the CPU keeps off the SRAM until it is done
localparam [20:0] FB_WORK = 21'h1f8000, FB_DISP0 = 21'h1f0000, FB_DISP1 = 21'h1e8000;
reg copy_pend = 1'b0, copy_run = 1'b0, dma_req = 1'b0;
wire dma_busy;
// Quake Ф3: quake_span fences only the z-buffer and the frames (>= 0x3b0000): the CPU builds surfaces meanwhile
wire d_fb = d_adr[19:0] >= 20'hEC000;
wire sr_ok = ~cpu_reset & ~copy_pend & ~drw_busy & ~(qsp_busy & d_fb);
assign copy_pend_w = copy_pend;
assign copy_run_w = copy_run;
assign dma_busy_w = dma_busy;
assign dbg_state = { copy_pend_w, copy_run_w, dma_busy_w, disp_front, disp_shown, timer_irq, ten, 1'b0 };
doom_sram sram (
	.clk(clk),
	.wb_cyc(d_req & d_sr & sr_ok), .wb_stb(d_req & d_sr & sr_ok), .wb_we(d_we), .wb_sel(d_sel),
	.wb_adr(d_adr[19:0]), .wb_cti(d_cti), .wb_dat_i(d_dat_o), .wb_dat_o(sr_dat), .wb_ack(sr_ack),
	.drw_req(drw_sr_req), .drw_adr(drw_sr_adr), .drw_sel(drw_sr_sel), .drw_dat(drw_sr_dat), .drw_ack(drw_sr_ack),
	.drw_we(drw_sr_we), .drw_rdat(drw_sr_rdat),
	.vid_req(vid_req), .vid_adr(vid_adr), .vid_len(vid_len), .vid_we(vid_we), .vid_wa(vid_wa),
	.vid_wd(vid_wd), .vid_busy(vid_busy),
	.dma_req(dma_req), .dma_src(FB_WORK), .dma_dst(disp_front ? FB_DISP0 : FB_DISP1), .dma_len(16'd32000),
	.dma_busy(dma_busy),
	.MA(MA), .MD_I(MD_I), .MD_O(MD_O), .MD_OE_N(MD_OE_N), .MWR_N(MWR_N), .MRD_N(MRD_N)
);

// ------------------------------------------------------------------ MMIO

reg [31:0] cycles = 32'd0;
reg [31:0] ms = 32'd0;
reg [9:0] ms_cnt = 10'd0;                 // microseconds of the current millisecond
reg [31:0] kbd_n = 32'd0;
reg signed [11:0] ms_dx = 12'sd0, ms_dy = 12'sd0;
reg [7:0] ms_btn = 8'd0;
reg io_ack = 1'b0;
reg [31:0] io_dat = 32'd0;
wire [13:0] io_off14 = { d_adr[11:0], 2'b00 };
wire [11:0] io_off = io_off14[11:0];
wire io_low = io_off14[13:12] == 2'b00;   // 0x0000..0x0fff: the registers here and the palette
// the registers of the top (doom_audio: the SFX ring 0x2000..0x3ffc, 0x60..0x7c; doom_sd: 0x80..0x9c,
// its sector buffer 0x1000..0x17fc): the aud_* bus
wire aud_sel = io_off14[13] || io_off14[13:11] == 3'b010 ||
	(io_off14[13:12] == 2'b00 && (io_off14[11:4] == 8'h06 || io_off14[11:4] == 8'h07 || io_off14[11:4] == 8'h08 || io_off14[11:4] == 8'h09));
assign aud_we = d_req && d_io && !io_ack && !cpu_reset && d_we && aud_sel;
// the drawer registers c0..dc; a queueing write (cc, d0) waits while its queue is full
wire drw_sel_io = io_low && io_off[11:8] == 4'h0 && io_off[7:5] == 3'b110;
// Quake Ф3: quake_span 0x100..0x13f; a SPAN / ZSPAN write waits while its queue is full
wire qsp_sel_io = WITH_QSPAN && io_low && io_off[11:6] == 6'b000100;
wire io_hold = (drw_sel_io && d_we && (io_off[7:0] == 8'hcc || io_off[7:0] == 8'hd0) && drw_push_wait) ||
	(qsp_sel_io && d_we && (io_off[5:0] == 6'h20 || io_off[5:0] == 6'h24) && qsp_push_wait);
wire qsp_we = d_req && d_io && !io_ack && !cpu_reset && d_we && qsp_sel_io && !io_hold;
wire drw_we = d_req && d_io && !io_ack && !cpu_reset && d_we && drw_sel_io && !io_hold;
generate if (WITH_QSPAN) begin : g_qsp
quake_span qsp (
	.clk(clk), .reset(cpu_reset),
	.io_we(qsp_we), .io_off({ 2'b00, io_off[5:2] }), .io_wd(d_dat_o), .push_wait(qsp_push_wait), .busy(qsp_busy),
	.sr_req(drw_sr_req), .sr_we(drw_sr_we), .sr_adr(drw_sr_adr), .sr_sel(drw_sr_sel), .sr_dat(drw_sr_dat),
	.sr_ack(drw_sr_ack), .sr_rdat(drw_sr_rdat)
);
assign drw_push_wait = 1'b0;
assign drw_busy = 1'b0;
assign drw_sd_cyc = 1'b0;
assign drw_sd_adr = 23'd0;
assign drw_sd_cti = 3'd0;
end else if (WITH_DRAW) begin : g_drw
doom_draw drw (
	.clk(clk), .reset(cpu_reset),
	.io_we(drw_we), .io_off(io_off[7:0]), .io_wd(d_dat_o), .push_wait(drw_push_wait), .busy(drw_busy),
	.sd_cyc(drw_sd_cyc), .sd_adr(drw_sd_adr), .sd_cti(drw_sd_cti), .sd_ack((own == O_DR) & sd_ack), .sd_dat(sd_dat),
	.sr_req(drw_sr_req), .sr_adr(drw_sr_adr), .sr_sel(drw_sr_sel), .sr_dat(drw_sr_dat), .sr_ack(drw_sr_ack)
);
assign qsp_busy = 1'b0;
assign qsp_push_wait = 1'b0;
assign drw_sr_we = 1'b1;
end else begin : g_nodrw
assign qsp_busy = 1'b0;
assign qsp_push_wait = 1'b0;
assign drw_sr_we = 1'b1;
assign drw_push_wait = 1'b0;
assign drw_busy = 1'b0;
assign drw_sd_cyc = 1'b0;
assign drw_sd_adr = 23'd0;
assign drw_sd_cti = 3'd0;
assign drw_sr_req = 1'b0;
assign drw_sr_adr = 21'd0;
assign drw_sr_sel = 2'd0;
assign drw_sr_dat = 16'd0;
end endgenerate
assign aud_off = io_off14;
assign aud_wd = d_dat_o;
reg [31:0] us = 32'd0, tcmp = 32'd0;
reg [9:0] us_div = 10'd0;                 // + CLK_DEN per clock, a microsecond at CLK_NUM
wire us_tick = us_div + CLK_DEN >= CLK_NUM;
reg ten = 1'b0;

always @(posedge clk) begin
	dma_req <= 1'b0;
	if (copy_pend && !copy_run && !dma_busy && !dma_req && !drw_busy && !qsp_busy && disp_shown == disp_front) begin
		dma_req <= 1'b1;
		copy_run <= 1'b1;
	end
	if (copy_run && !dma_req && !dma_busy) begin
		copy_run <= 1'b0;
		copy_pend <= 1'b0;
		disp_front <= ~disp_front;
	end
	if (d_req && d_io && !io_ack && !cpu_reset && d_we && io_off14 == 14'h0004) copy_pend <= 1'b1;
end


function signed [11:0] sat_add(input signed [11:0] a, input [7:0] d);
	reg signed [12:0] s;
	begin
		s = { a[11], a } + { {5{d[7]}}, d };
		sat_add = (s > 13'sd2047) ? 12'sd2047 : (s < -13'sd2048) ? -12'sd2048 : s[11:0];
	end
endfunction

always @(posedge clk) begin
	io_ack <= 1'b0;
	pal_we <= 1'b0;
	timer_irq <= ten && !cpu_reset && $signed(us - tcmp) >= 0;
	if (cpu_reset) begin
		us <= 32'd0;
		us_div <= 10'd0;
		ten <= 1'b0;
	end else if (us_tick) begin
		us_div <= us_div + CLK_DEN - CLK_NUM;
		us <= us + 32'd1;
	end else
		us_div <= us_div + CLK_DEN;
	if (cpu_reset) begin
		cycles <= 32'd0;
		ms <= 32'd0;
		ms_cnt <= 10'd0;
	end else begin
		cycles <= cycles + 32'd1;
		if (us_tick) begin
			if (ms_cnt == 10'd999) begin ms_cnt <= 10'd0; ms <= ms + 32'd1; end
			else ms_cnt <= ms_cnt + 10'd1;
		end
	end
	if (kbd_wr) kbd_n <= kbd_n + 32'd1;
	if (ms_wr) begin
		ms_dx <= sat_add(ms_dx, ms_x);
		ms_dy <= sat_add(ms_dy, ms_y);
		ms_btn <= ms_b;
	end

	if (d_req && d_io && !io_ack && !cpu_reset && !io_hold) begin
		io_ack <= 1'b1;
		if (d_we) begin
			if (!io_low || aud_sel) begin
				// doom_audio (aud_we)
			end else if (qsp_sel_io) begin
				// quake_span (qsp_we)
			end else if (io_off[11:10] == 2'b01) begin   // 0x400..0x7fc
				pal_we <= 1'b1;
				pal_wa <= io_off[9:2];
				pal_wd <= d_dat_o[23:0];
			end else case (io_off[7:0])
				8'h00: dbg_uart <= d_dat_o[7:0];
				8'h04: frame_n <= frame_n + 16'd1;
				8'h08: dbg_exit <= { 1'b1, d_dat_o[7:0] };
				8'h44: begin tcmp <= d_dat_o; dbg_tcmp_n <= dbg_tcmp_n + 8'd1; end
				8'h48: ten <= d_dat_o[0];
				default: ;
			endcase
		end else if (aud_sel) begin
			// the registers of the top (the sector buffer 0x1000.. too): not by the low byte below
			io_dat <= aud_rd;
		end else if (qsp_sel_io) begin
			io_dat <= io_off[5:0] == 6'h3c ? { 8'h95, 23'd0, qsp_busy } : 32'd0;   // Quake Ф3: STAT
		end else begin
			case (io_off[7:0])
				8'h0c: io_dat <= cycles;
				8'h1c: io_dat <= ms;
				8'h20: io_dat <= wad_size;
				8'h28: io_dat <= kbd[31:0];
				8'h2c: io_dat <= { 8'd0, kbd[55:32] };
				8'h30: io_dat <= kbd_n;
				8'h34: begin
					io_dat <= { ms_btn, ms_dy, ms_dx };
					if (ms_wr) begin
						ms_dx <= sat_add(12'sd0, ms_x);
						ms_dy <= sat_add(12'sd0, ms_y);
					end else begin
						ms_dx <= 12'sd0;
						ms_dy <= 12'sd0;
					end
				end
				8'h38: io_dat <= { joy_r, joy_l };
				8'h40: io_dat <= us;
				8'h50: io_dat <= sw;
				8'hdc: io_dat <= WITH_DRAW ? { 8'hd7, 23'd0, drw_busy } : 32'd0;
				default: io_dat <= aud_sel ? aud_rd : 32'd0;
			endcase
		end
	end

	// loader
	if (ld_start && ld_is_wad) begin wad_size <= 32'd0; wad_top <= 32'd0; end
	if (ld_wr && !ld_pend) begin
		ld_pend <= 1'b1;
		ld_d <= ld_data;
		ld_a <= (ld_kind == 2'd0) ? ld_addr[24:0] : 25'hc00000 + ld_addr[24:0];
		if (ld_is_wad && ld_addr + 32'd1 > wad_top) wad_top <= ld_addr + 32'd1;
	end
	if (own == O_LD && sd_ack) ld_pend <= 1'b0;
	if (ld_end && ld_is_wad) wad_size <= wad_top;
end

// ------------------------------------------------------------------ CPU bus answers

always @(*) begin
	i_ack = (own == O_I) & sd_ack;
	i_dat_i = sd_dat;
	d_ack = ((own == O_D) & sd_ack) | sr_ack | io_ack;
	d_dat_i = d_io ? io_dat : d_sd ? sd_dat : sr_dat;
end

endmodule
