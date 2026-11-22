`timescale 1ns / 1ps
`default_nettype none
/*
 quake_span.v (karabas, Quake Ф3)

 The span drawer of the Quake core: the inner loops of D_DrawSpans8 / D_DrawZSpans (d_scan.c, KG_HWSPAN) in the
 FPGA. The texture (the surface cache block) and the frame / z-buffer are in the SRAM; every access goes through
 the drawer port of doom_sram (a halfword read or a lane write, one at a time). Registers (MMIO 0x100..0x13c,
 src/fw/mmio.h; io_off = the offset from 0x100):
   00 TEX  SRAM byte address of the texture (21..0; bit 28 = SDRAM is not supported here)
   04 TW   its row bytes (cachewidth, 11..0)
   08 S, 0c T, 10 SSTEP, 14 TSTEP: 16.16 texel position and steps
   18 IZI, 1c IZISTEP: 1/z 16.16 (z = izi >> 16)
   20 SPAN  = { count - 1 (24..16), pixel offset in the frame (15..0) }: queues count texels into the frame
   24 ZSPAN = the same for count z halfwords into the z-buffer
 A queued command keeps the register values of its write (a queue of QN entries in distributed RAM, written
 once per clock: the XST LUTRAM rule). push_wait: SPAN / ZSPAN while the queue is full (doom_soc holds the
 write). busy: a command queued or running (doom_soc: the CPU waits for the frame / z-buffer SRAM and the frame
 copy). Functional model: src/sim/qspan_model.h.

 A pixel: the texel address tex + (s >> 16) + (t >> 16) * tw, a halfword read, its byte written into the frame
 byte FB + off (lane off[0]); then s += sstep, t += tstep. A z: the halfword izi >> 16 to ZB + 2 * off.
*/
module quake_span #(
	parameter [21:0] FB = 22'h3f0000,     // the work frame (byte address in the SRAM)
	parameter [21:0] ZB = 22'h3b0000      // the z-buffer
) (
	input wire clk,
	input wire reset,

	input wire io_we,
	input wire [5:0] io_off,              // (MMIO offset - 0x100) / 4: the register
	input wire [31:0] io_wd,
	output wire push_wait,
	output wire busy,

	// doom_sram drawer port
	output reg sr_req = 1'b0,
	output reg sr_we = 1'b0,
	output reg [20:0] sr_adr = 21'd0,
	output reg [1:0] sr_sel = 2'd0,
	output reg [15:0] sr_dat = 16'd0,
	input wire sr_ack,
	input wire [15:0] sr_rdat
);

localparam QN = 4;

// ------------------------------------------------------------------ registers
reg [21:0] tex = 22'd0;
reg [11:0] tw = 12'd0;
reg [31:0] r_s = 32'd0, r_t = 32'd0, r_ss = 32'd0, r_ts = 32'd0, r_izi = 32'd0, r_izs = 32'd0;

// ------------------------------------------------------------------ the queue
// entry: { z, n-1 (9), off (16), tex (22), tw (12), a (32), b (32), c (32), d (32) }:
// a span: a = s, b = t, c = sstep, d = tstep; a z span: a = izi, c = izistep
localparam EW = 1 + 9 + 16 + 22 + 12 + 128;
reg [EW-1:0] q_mem [0:QN-1];
reg [1:0] q_wp = 2'd0, q_rp = 2'd0;
reg [2:0] q_cnt = 3'd0;
wire push = io_we && (io_off == 6'h08 || io_off == 6'h09) && q_cnt != QN;
wire is_z = io_off[0];
wire [EW-1:0] q_in = { is_z, io_wd[24:16], io_wd[15:0], tex, tw,
	is_z ? r_izi : r_s, r_t, is_z ? r_izs : r_ss, r_ts };
reg pop = 1'b0;
assign push_wait = q_cnt == QN;

always @(posedge clk) begin
	if (push) q_mem[q_wp] <= q_in;
end

always @(posedge clk) begin
	if (reset) begin
		q_wp <= 2'd0; q_rp <= 2'd0; q_cnt <= 3'd0;
	end else begin
		if (push) q_wp <= q_wp + 2'd1;
		if (pop) q_rp <= q_rp + 2'd1;
		q_cnt <= q_cnt + (push ? 3'd1 : 3'd0) - (pop ? 3'd1 : 3'd0);
	end
	if (io_we) case (io_off)
		6'h00: tex <= io_wd[21:0];
		6'h01: tw <= io_wd[11:0];
		6'h02: r_s <= io_wd;
		6'h03: r_t <= io_wd;
		6'h04: r_ss <= io_wd;
		6'h05: r_ts <= io_wd;
		6'h06: r_izi <= io_wd;
		6'h07: r_izs <= io_wd;
		default: ;
	endcase
end

// ------------------------------------------------------------------ the engine
localparam [2:0] E_IDLE = 3'd0, E_LOAD = 3'd1, E_ADDR = 3'd2, E_TRD = 3'd3, E_PWR = 3'd4, E_ZWR = 3'd5;
reg [2:0] es = E_IDLE;
reg c_z = 1'b0;
reg [8:0] c_n = 9'd0;                     // pixels left - 1
reg [15:0] c_off = 16'd0;
reg [21:0] c_tex = 22'd0;
reg [11:0] c_tw = 12'd0;
reg [31:0] c_a = 32'd0, c_b = 32'd0, c_c = 32'd0, c_d = 32'd0;
reg [21:0] t_adr = 22'd0;                 // the texel byte address
wire [EW-1:0] q_head = q_mem[q_rp];

// (t >> 16) * tw: 16 x 12 bits (a DSP48); s >> 16 signed is clamped >= 0 by the engine (d_scan.c clamps)
wire [27:0] row_off = c_b[31:16] * c_tw;

assign busy = q_cnt != 3'd0 || es != E_IDLE;

always @(posedge clk) begin
	pop <= 1'b0;
	if (reset) begin
		es <= E_IDLE;
		sr_req <= 1'b0;
	end else case (es)
	E_IDLE: if (q_cnt != 3'd0 && !pop) es <= E_LOAD;
	E_LOAD: begin
		{ c_z, c_n, c_off, c_tex, c_tw, c_a, c_b, c_c, c_d } <= q_head;
		pop <= 1'b1;
		es <= q_head[EW-1] ? E_ZWR : E_ADDR;
	end
	E_ADDR: begin
		t_adr <= c_tex + { 6'd0, c_a[31:16] } + row_off[21:0];
		es <= E_TRD;
	end
	E_TRD: begin
		// the texel's halfword
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b0; sr_adr <= t_adr[21:1];
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			sr_dat <= t_adr[0] ? { sr_rdat[15:8], sr_rdat[15:8] } : { sr_rdat[7:0], sr_rdat[7:0] };
			es <= E_PWR;
		end
	end
	E_PWR: begin
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b1;
			sr_adr <= (FB[21:1] + { 5'd0, c_off[15:1] });
			sr_sel <= c_off[0] ? 2'b10 : 2'b01;
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			c_a <= c_a + c_c;
			c_b <= c_b + c_d;
			c_off <= c_off + 16'd1;
			c_n <= c_n - 9'd1;
			es <= (c_n == 9'd0) ? E_IDLE : E_ADDR;
		end
	end
	E_ZWR: begin
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b1;
			sr_adr <= ZB[21:1] + { 5'd0, c_off };
			sr_sel <= 2'b11;
			sr_dat <= c_a[31:16];
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			c_a <= c_a + c_c;
			c_off <= c_off + 16'd1;
			c_n <= c_n - 9'd1;
			if (c_n == 9'd0) es <= E_IDLE;
		end
	end
	default: es <= E_IDLE;
	endcase
end

endmodule
