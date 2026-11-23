`timescale 1ns / 1ps
`default_nettype none
/*
 quake_span.v (karabas, Quake Ф3: the span drawer; Ф3е: with the perspective; Ф7: the polygon rasterizer)

 The inner work of D_DrawSpans8 + D_DrawZSpans (d_scan.c, KG_HWSPAN + KG_HWPERSP) in the FPGA, and (Ф7) the faces of
 the polygon driver (D_DrawPoly, d_edge.c KG_POLY). The texture (the surface cache block), the frame, the z-buffer
 and the coverage mask are in the SRAM, reached through the drawer port of doom_sram (a halfword read or a lane
 write, one at a time). Functional model, bit for bit: src/sim/qspan_model.h.

 Registers (MMIO 0x100..0x1fc, src/fw/mmio.h; io_off = (offset - 0x100) / 4):
   00 TEX  SRAM byte address of the texture (21..0)        01 TW   its row bytes (cachewidth, 11..0)
   06 IZI, 07 IZISTEP: 1/z 16.16 of a z span (z = izi >> 16)
   09 ZSPAN  = { count - 1 (24..16), pixel offset (15..0) }: count z halfwords into the z-buffer
   16..24 SO SU SV TO TU TV ZO ZU ZV: the surface, S = sdivz * 2^ks, T = tdivz * 2^kt, Z = zi * 2^kz, each
          origin + u * grad_u + v * grad_v at screen pixel (u, v)
   25 K = { kz (23..16), kt (15..8), ks (7..0) } signed      26 SADJ, 27 TADJ (sadjust, tadjust)
   28 BBS, 29 BBT (bbextents, bbextentt)
   30 PSPAN = { count - 1 (25..17), v (16..9), u (8..0) }: a perspective span (texels and z)
   32 PVTX = { v (31..16), u (15..0) }: a polygon vertex, signed 12.4 (at most 64 per polygon)
   33 PDRAW: the convex polygon of the vertices since the last PDRAW with the surface above; bit 0 = 0 (the world):
          a pixel only where the coverage mask is 0, 1 (bmodels): where its z >= the z-buffer one (signed)
   34 ZCLR: the coverage mask to 0; from then on ZSPAN marks its pixels in the mask too
 Ф7: every write goes into the command FIFO (512 entries, BRAM); io_hold only when it is full. The command part
 takes them in order, one per clock: a TEX / TW / K / surface register write waits until the queued spans are
 through the math part (its descriptors carry the texture), PVTX / PDRAW / spans until the rasterizer is idle.
 busy: work queued or running (doom_soc: the CPU waits for the frame / z-buffer SRAM and the frame copy).

 The rasterizer (PDRAW): per edge (both ends sorted by v; a horizontal one skipped) its rows ceil(Ya / 16) ..
 ceil(Yb / 16) - 1, step = trunc((Xb - Xa) * 65536 / (Yb - Ya)) (a 30-step divider), x of its first row
 Xa * 4096 + (((16 * yf - Ya) * step) >>> 4) in 2^-16 pixels, then + step per row (= qspan_model.h edge_x).
 Per row: x of the edges covering it, xl = min, xr = max (two at least), pixels ceil(xl) .. ceil(xr) - 1 clamped
 to 0..320, rows 0..199: a span into the queue with the test of the polygon.
 The coverage mask (QSP_MASK, a bit per pixel, bit k of halfword i = pixel 16 i + k): the pixel part holds one
 halfword (16 pixels), reads it when the pixel leaves it, writes it back when changed (and at the end of a span:
 the mask is right whenever the drawer is idle). A drawn pixel of a tested span sets its bit; ZCLR = 8 spans
 writing 0 to the mask (500 halfwords each).

 The math part takes the spans. A z span goes to the pixel part as one descriptor. A perspective span: S, T,
 Z at (u, v) (origin * 1 + grad_u * u + grad_v * v through the one multiplier, summed per value), then s, t at the span start and at the far end of each 16-pixel
 segment (the last one: of its last pixel): Z = m * 2^(e - 23), r = floor(2^47 / m) (a 25-step divider),
 q = sat32((S * r) >> (ks - kz + 8 + e)) (a left shift for a negative amount), s = q + sadjust clamped as d_scan.c
 (the upper bound first); the steps (>> 4, or the reciprocal table on the last segment); one descriptor per
 segment. The pixel part draws a descriptor while the math part works on the next one: per pixel (a tested
 span: the mask / the z first) the texel (a halfword read unless it is the halfword read last), the pixel (two
 neighbours of a span in one halfword write), the z halfword (Z >> (kz - 15), at least 1 in a tested span; or
 izi >> 16 of a z span).
 Ф5 (area): the surface registers 16..29 are a distributed RAM (one write port, one read port for the math part);
 ks / kt / kz are kept in registers too. The per-segment S, T, Z steps (grad_u * 16, or * (seg - 1)) go through
 the multiplier as well.
*/
module quake_span #(
	parameter [21:0] FB = 22'h3f0000,     // the work frame (byte address in the SRAM)
	parameter [21:0] ZB = 22'h3b0000,     // the z-buffer
	parameter [21:0] MASK = 22'h1fe000    // Ф7: the coverage mask (8000 bytes)
) (
	input wire clk,
	input wire reset,

	input wire io_sel,                    // a write to 0x100..0x1fc is presented (doom_soc)
	input wire [5:0] io_off,
	input wire [31:0] io_wd,
	output wire io_hold,
	input wire io_we,                     // the write is taken (io_sel and no hold in doom_soc)
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

// ------------------------------------------------------------------ the command FIFO (Ф7: BRAM, 512 x 38)
reg [37:0] cf_mem [0:511];
reg [8:0] cf_wp = 9'd0, cf_rp = 9'd0;
reg [9:0] cf_cnt = 10'd0;
reg [37:0] cf_q = 38'd0;                  // cf_mem[cf_rp] of the last clock
reg cf_ok = 1'b0;                         // cf_q is the head (first word fall through: read at the next address)
wire cf_full = cf_cnt >= 10'd510;
wire cf_push = io_we;                     // doom_soc: io_sel and no io_hold
wire cf_pop;                              // the command part takes the head in this clock
wire [8:0] cf_rp_n = cf_rp + (cf_pop ? 9'd1 : 9'd0);
always @(posedge clk) begin
	if (cf_push) cf_mem[cf_wp] <= { io_off, io_wd };
	cf_q <= cf_mem[cf_rp_n];
end
always @(posedge clk) begin
	if (reset) begin
		cf_wp <= 9'd0; cf_rp <= 9'd0; cf_cnt <= 10'd0; cf_ok <= 1'b0;
	end else begin
		if (cf_push) cf_wp <= cf_wp + 9'd1;
		cf_rp <= cf_rp_n;
		cf_cnt <= cf_cnt + (cf_push ? 10'd1 : 10'd0) - (cf_pop ? 10'd1 : 10'd0);
		// the entry at cf_rp_n was written before this clock
		cf_ok <= cf_cnt - (cf_pop ? 10'd1 : 10'd0) != 10'd0;
	end
end
wire [5:0] c_op = cf_q[37:32];
wire [31:0] c_wd = cf_q[31:0];

// ------------------------------------------------------------------ registers
reg [21:0] tex = 22'd0;
reg [11:0] tw = 12'd0;
reg [31:0] r_izi = 32'd0, r_izs = 32'd0;
reg signed [7:0] ks = 0, kt = 0, kz = 0;
reg polymode = 1'b0;                      // a ZCLR was taken: ZSPAN marks the mask too
// the surface registers 16..29 (index c_op - 16): a distributed RAM, read by the math part (pr_ra below)
localparam [3:0] R_ZU = 4'd7, R_SADJ = 4'd10, R_TADJ = 4'd11, R_BBS = 4'd12, R_BBT = 4'd13;
reg [31:0] pr_mem [0:15];
reg [3:0] pr_ra;
wire [31:0] pr_rd = pr_mem[pr_ra];
wire c_reg_we;                            // the command part writes a register (c_op, c_wd) in this clock
always @(posedge clk) begin
	if (c_reg_we && c_op[5:4] == 2'b01 && c_op[3:0] <= 4'd13) pr_mem[c_op[3:0]] <= c_wd;
end
always @(posedge clk) begin
	if (c_reg_we) case (c_op)
		6'd0: tex <= c_wd[21:0];
		6'd1: tw <= c_wd[11:0];
		6'd6: r_izi <= c_wd;
		6'd7: r_izs <= c_wd;
		6'd25: begin ks <= c_wd[7:0]; kt <= c_wd[15:8]; kz <= c_wd[23:16]; end
		default: ;
	endcase
end

// ------------------------------------------------------------------ the span queue (distributed RAM, one write per clock)
// { p, mode (2), n - 1, uv / off, izi, izistep }; mode of a perspective span: 0 plain, 1 the mask, 2 the z;
// of a z span: 0 a z span, 3 a mask clear (writes 0 to the mask halfwords)
localparam QN = 4;
localparam EW = 1 + 2 + 9 + 17 + 64;
reg [EW-1:0] q_mem [0:QN-1];
reg [1:0] q_wp = 2'd0, q_rp = 2'd0;
reg [2:0] q_cnt = 3'd0;
wire q_full = q_cnt == QN;
reg q_push = 1'b0;
reg [EW-1:0] q_in = {EW{1'b0}};
reg pop = 1'b0;
wire [EW-1:0] q_head = q_mem[q_rp];

always @(posedge clk) begin
	if (q_push) q_mem[q_wp] <= q_in;
end
always @(posedge clk) begin
	if (reset) begin
		q_wp <= 2'd0; q_rp <= 2'd0; q_cnt <= 3'd0;
	end else begin
		if (q_push) q_wp <= q_wp + 2'd1;
		if (pop) q_rp <= q_rp + 2'd1;
		q_cnt <= q_cnt + (q_push ? 3'd1 : 3'd0) - (pop ? 3'd1 : 3'd0);
	end
end

// ------------------------------------------------------------------ the command part (Ф7)
localparam [3:0] R_IDLE = 4'd0, R_RA = 4'd1, R_RB = 4'd2, R_SORT = 4'd3, R_PREP = 4'd4, R_DIV = 4'd5,
	R_STEP = 4'd6, R_X0W = 4'd7, R_NEXT = 4'd8, R_ROW = 4'd9, R_SPAN = 4'd10;
reg [3:0] rs = R_IDLE;
reg [6:0] vn = 7'd0;                      // vertices written since the last PDRAW
reg [2:0] zc_k = 3'd0;                    // ZCLR: the mask-clear spans queued
wire core_idle;                           // no span queued, the math / pixel parts idle (below)
wire q_room = q_cnt + (q_push ? 3'd1 : 3'd0) < QN;
wire c_is_reg = c_op == 6'd0 || c_op == 6'd1 || (c_op >= 6'd16 && c_op <= 6'd29);
wire c_ready = cf_ok;
wire c_go = c_ready && (
	c_is_reg ? (q_cnt == 3'd0 && ms == M_IDLE && rs == R_IDLE && !r_start && !q_push) :
	(c_op == 6'd9 || c_op == 6'd30 || c_op == 6'd34) ? (rs == R_IDLE && q_room) :
	(c_op == 6'd32 || c_op == 6'd33) ? (rs == R_IDLE && !r_start) : 1'b1);
// ZCLR stays the head until its 8th span is queued
wire c_take = c_go && (c_op != 6'd34 || zc_k == 3'd7);
assign cf_pop = c_take;
assign c_reg_we = c_go && (c_is_reg || c_op == 6'd6 || c_op == 6'd7);

// the vertices: { v, u } 12.4 (written at vn, read at r_i / r_j by the rasterizer)
reg [31:0] vx_mem [0:63];
reg [5:0] r_i = 6'd0;                     // the edge (from vertex r_i to r_j)
wire r_lastv = r_i + 6'd1 == r_n[5:0] || (r_n == 7'd64 && r_i == 6'd63);
wire [5:0] r_j = r_lastv ? 6'd0 : r_i + 6'd1;
wire [5:0] vx_ra = rs == R_RB ? r_j : r_i;
wire [31:0] vx_rd = vx_mem[vx_ra];
always @(posedge clk) begin
	if (c_go && c_op == 6'd32 && !vn[6]) vx_mem[vn[5:0]] <= c_wd;
end

reg r_start = 1'b0;
reg [6:0] r_n = 7'd0;                     // vertices of the polygon being drawn
reg [1:0] r_mode = 2'd1;
wire r_push;
reg [8:0] r_cnt1_q = 9'd0;
reg signed [9:0] r_y = 0;
wire [9:0] r_xs;
wire [8:0] r_cnt1;

always @(posedge clk) begin
	r_start <= 1'b0;
	q_push <= 1'b0;
	if (reset) begin
		vn <= 7'd0; zc_k <= 3'd0; polymode <= 1'b0;
	end else begin
		if (c_go) case (c_op)
			6'd9: begin
				q_push <= 1'b1;
				q_in <= { 1'b0, 2'd0, c_wd[24:16], 1'b0, c_wd[15:0], r_izi, r_izs };
			end
			6'd30: begin
				q_push <= 1'b1;
				q_in <= { 1'b1, 2'd0, c_wd[25:17], c_wd[16:0], 64'd0 };
			end
			6'd32: if (!vn[6]) vn <= vn + 7'd1;
			6'd33: begin
				r_start <= 1'b1; r_n <= vn; r_mode <= c_wd[0] ? 2'd2 : 2'd1;
				vn <= 7'd0;
			end
			6'd34: begin
				q_push <= 1'b1;
				q_in <= { 1'b0, 2'd3, 9'd499, 1'b0, { 7'd0, zc_k } * 16'd500, 64'd0 };
				zc_k <= zc_k + 3'd1;
				polymode <= 1'b1;
			end
			default: ;
		endcase
		if (r_push) begin                 // (never with a push of the command part: the rasterizer is busy)
			q_push <= 1'b1;
			q_in <= { 1'b1, r_mode, r_cnt1, r_y[7:0], r_xs[8:0], 64'd0 };
		end
	end
end

// ------------------------------------------------------------------ the rasterizer (Ф7, qspan_model.h poly())
// the edges: { active, yf, yl, step, x } (x: of the next row of the edge, 2^-16 pixels); read and written at r_i
reg [84:0] e_mem [0:63];
wire [84:0] e_rd = e_mem[r_i];
reg signed [15:0] r_xa = 0, r_ya = 0, r_xb = 0, r_yb = 0;
reg signed [9:0] r_yf = 0, r_yl = 0;
reg signed [9:0] r_ymin = 0, r_ymax = 0;
// the divider: |Xb - Xa| * 65536 / (Yb - Ya), 30 quotient bits (|Xb - Xa| < 2^14: Quake clamps to the screen)
reg [29:0] dv_n = 30'd0;                  // the dividend, shifting out at the top
reg [16:0] dv_r = 17'd0;                  // the remainder (< the divisor < 2^16)
reg [29:0] dv_qq = 30'd0;
reg [16:0] dv_d = 17'd0;
reg dv_neg = 1'b0;
reg [4:0] dv_k = 5'd0;
wire [17:0] dv_t = { dv_r, dv_n[29] } - { 1'b0, dv_d };
reg signed [31:0] r_step = 0;
wire [3:0] r_fr = 4'd0 - r_ya[3:0];      // 16 * yf - Ya (0..15)
wire signed [36:0] r_fs = $signed({ 1'b0, r_fr }) * r_step;
wire signed [31:0] r_x0 = $signed({ {4{r_xa[15]}}, r_xa, 12'd0 }) + r_fs[35:4];
wire signed [15:0] r_yf16 = (r_ya + 16'sd15) >>> 4, r_yl16 = (r_yb + 16'sd15) >>> 4;
// a row
reg signed [31:0] r_xl = 0, r_xr = 0;
reg [1:0] r_found = 2'd0;
wire e_act = e_rd[84];
wire signed [9:0] e_yf = e_rd[83:74], e_yl = e_rd[73:64];
wire signed [31:0] e_step = e_rd[63:32], e_x = e_rd[31:0];
wire e_on = e_act && r_y >= e_yf && r_y < e_yl;
wire signed [16:0] r_xs_c = ({ r_xl[31], r_xl } + 33'sh0ffff) >>> 16;
wire signed [16:0] r_xe_c = ({ r_xr[31], r_xr } + 33'sh0ffff) >>> 16;
assign r_xs = r_xs_c < 0 ? 10'd0 : r_xs_c > 17'sd320 ? 10'd320 : r_xs_c[9:0];
wire [9:0] r_xe = r_xe_c < 0 ? 10'd0 : r_xe_c > 17'sd320 ? 10'd320 : r_xe_c[9:0];
wire [9:0] r_cnt = r_xe - r_xs;
assign r_cnt1 = r_cnt[8:0] - 9'd1;
wire r_draw = r_found == 2'd2 && r_xs < r_xe && r_y >= 0 && r_y < 10'sd200;
assign r_push = rs == R_SPAN && r_draw && q_room;

always @(posedge clk) begin
	if (reset) rs <= R_IDLE;
	else case (rs)
	R_IDLE: if (r_start) begin
		r_i <= 6'd0;
		r_ymin <= 10'sd511; r_ymax <= -10'sd512;
		rs <= r_n >= 7'd3 ? R_RA : R_IDLE;
	end
	R_RA: begin r_xa <= vx_rd[15:0]; r_ya <= vx_rd[31:16]; rs <= R_RB; end
	R_RB: begin r_xb <= vx_rd[15:0]; r_yb <= vx_rd[31:16]; rs <= R_SORT; end
	R_SORT: begin                         // both ends by v (the lower first): the two faces of an edge get the same numbers
		if (r_ya > r_yb) begin
			r_xa <= r_xb; r_ya <= r_yb; r_xb <= r_xa; r_yb <= r_ya;
		end
		rs <= R_PREP;
	end
	R_PREP: begin
		r_yf <= r_yf16[9:0];
		r_yl <= r_yl16[9:0];
		dv_neg <= r_xb < r_xa;
		dv_n <= (r_xb < r_xa ? { 14'd0, r_xa - r_xb } : { 14'd0, r_xb - r_xa }) << 16;
		dv_d <= { 1'b0, r_yb - r_ya };
		dv_r <= 17'd0;
		dv_k <= 5'd29;
		if (r_yf16 >= r_yl16) begin         // no row (a horizontal edge too): inactive
			e_mem[r_i] <= 85'd0;
			rs <= R_NEXT;
		end else
			rs <= R_DIV;
	end
	R_DIV: begin
		if (!dv_t[17]) begin dv_r <= dv_t[16:0]; dv_qq <= { dv_qq[28:0], 1'b1 }; end
		else begin dv_r <= { dv_r[15:0], dv_n[29] }; dv_qq <= { dv_qq[28:0], 1'b0 }; end
		dv_n <= { dv_n[28:0], 1'b0 };
		dv_k <= dv_k - 5'd1;
		if (dv_k == 5'd0) rs <= R_STEP;
	end
	R_STEP: begin                         // the step truncated toward 0 (C)
		r_step <= dv_neg ? -$signed({ 2'b00, dv_qq }) : $signed({ 2'b00, dv_qq });
		rs <= R_X0W;
	end
	R_X0W: begin
		e_mem[r_i] <= { 1'b1, r_yf, r_yl, r_step, r_x0 };
		if (r_yf < r_ymin) r_ymin <= r_yf;
		if (r_yl > r_ymax) r_ymax <= r_yl;
		rs <= R_NEXT;
	end
	R_NEXT: begin
		if (r_lastv) begin
			r_i <= 6'd0;
			r_y <= r_ymin;
			r_xl <= 32'sh7FFFFFFF; r_xr <= 32'sh80000000; r_found <= 2'd0;
			rs <= r_ymin < r_ymax ? R_ROW : R_IDLE;
		end else begin
			r_i <= r_i + 6'd1;
			rs <= R_RA;
		end
	end
	// a row: the edges one per clock (x, then x + step back), then the span
	R_ROW: begin
		if (e_on) begin
			e_mem[r_i] <= { e_rd[84:32], e_x + e_step };
			if (e_x < r_xl) r_xl <= e_x;
			if (e_x > r_xr) r_xr <= e_x;
			if (r_found != 2'd2) r_found <= r_found + 2'd1;
		end
		if (r_lastv) rs <= R_SPAN;
		else r_i <= r_i + 6'd1;
	end
	R_SPAN: if (r_push || !r_draw) begin
		r_i <= 6'd0;
		r_xl <= 32'sh7FFFFFFF; r_xr <= 32'sh80000000; r_found <= 2'd0;
		r_y <= r_y + 10'sd1;
		rs <= r_y + 10'sd1 < r_ymax ? R_ROW : R_IDLE;
	end
	default: rs <= R_IDLE;
	endcase
end

// ------------------------------------------------------------------ the descriptor (math -> pixel)
reg d_valid = 1'b0;
reg d_z = 1'b0;                           // a z span (no texels)
reg [1:0] d_mode = 2'd0;                  // Ф7: the test of a perspective span / a z span or a mask clear (the queue)
reg d_first = 1'b0, d_last = 1'b0;        // first / last segment of a perspective span
reg [9:0] d_n = 10'd0;                    // pixels (1..512)
reg [15:0] d_off = 16'd0;
reg [21:0] d_tex = 22'd0;                 // Ф7: the texture of the descriptor (the registers may change after it)
reg [11:0] d_tw = 12'd0;
reg signed [31:0] d_s = 0, d_t = 0, d_ss = 0, d_ts = 0;
reg [31:0] d_zacc = 32'd0, d_zstep = 32'd0;
reg signed [7:0] d_zsh = 0;
reg d_take = 1'b0;                        // the pixel part takes the descriptor (a pulse)

// ------------------------------------------------------------------ the math part
localparam [4:0] M_IDLE = 5'd0, M_POP = 5'd1, M_ZEMIT = 5'd2, M_ACC = 5'd3, M_MCL2 = 5'd4,
	M_NORM = 5'd6, M_DIV = 5'd7, M_MS = 5'd8, M_MSW1 = 5'd9, M_MSW2 = 5'd10, M_MSQ = 5'd11, M_MTQ = 5'd12,
	M_MCL = 5'd13, M_AFTER = 5'd14, M_SEG = 5'd15, M_STEP = 5'd18, M_SW1 = 5'd19,
	M_SW2 = 5'd20, M_SWQ = 5'd21, M_SWQ2 = 5'd22, M_EMIT = 5'd23, M_NORM2 = 5'd24, M_MSW3 = 5'd25,
	M_MSW4 = 5'd26, M_MSW5 = 5'd27, M_MSW6 = 5'd28, M_SW3 = 5'd29, M_SW4 = 5'd30;
reg [4:0] ms = M_IDLE;
reg [9:0] m_left = 10'd0;                 // pixels of the span not yet in a segment
reg [4:0] m_seg = 5'd0;                   // pixels of the current segment
reg [15:0] m_off = 16'd0;
reg [7:0] m_v = 8'd0;
reg signed [31:0] mS = 0, mT = 0, mZ = 0, mZ0 = 0;
reg signed [31:0] m_s = 0, m_t = 0;       // s, t at the start of the segment
reg signed [31:0] m_sn = 0, m_tn = 0;     // at its end (the next start)
reg signed [31:0] m_ss = 0, m_ts = 0;
reg m_first = 1'b0;                       // the first segment of the span is next
reg m_init = 1'b0;                        // evaluating the span start (the lower bound 0)
reg [1:0] m_mode = 2'd0;                  // Ф7: the test of the span (the queue entry)
reg [9:0] m_zn = 10'd0;                   // a z span
reg [15:0] m_zoff = 16'd0;
reg [31:0] m_zizi = 32'd0, m_zizs = 32'd0;

// the accumulate pass (M_ACC): products register * b through the multiplier below, one issued per clock, each
// added to S / T / Z (ac_ch) when it comes out (5 clocks later, the tag pipe tg_*). Group 0: the origin * 1,
// 1: grad_u * ac_bu (u, 16 or seg - 1), 2: grad_v * v; the register index = 3 * value + group.
reg [1:0] ac_ch = 2'd0, ac_grp = 2'd0;
reg ac_lg = 1'b0;                         // the last group: 0 = group 1 (a segment), 1 = group 2 (the span start)
reg ac_iss = 1'b0;                        // products still to issue
reg [9:0] ac_bu = 10'd0;
reg [4:0] tg_v = 5'd0;
reg [9:0] tg_ch = 10'd0;
wire ac_issue = ms == M_ACC && ac_iss;
always @(posedge clk) begin
	tg_v <= reset ? 5'd0 : { tg_v[3:0], ac_issue };
	tg_ch <= { tg_ch[7:0], ac_ch };
end
always @(*) begin
	case (ms)
	M_ACC: pr_ra = { ac_ch, 1'b0 } + { 2'b00, ac_ch } + { 2'b00, ac_grp };
	M_MSW2: pr_ra = R_SADJ;
	M_MSW3: pr_ra = R_BBS;
	M_MTQ: pr_ra = R_TADJ;
	M_MCL: pr_ra = R_BBT;
	default: pr_ra = R_ZU;                // M_EMIT
	endcase
end

// Z normalised: e = the top bit, m = its 24 bits from there
// Ф5: the top set bit as a two-level tree (8-bit groups; a plain loop made a long chain of multiplexers)
function [3:0] pe8;                       // { any, the top set bit } of 8 bits
	input [7:0] x;
	integer k;
	begin
		pe8 = 4'd0;
		for (k = 0; k < 8; k = k + 1)
			if (x[k]) pe8 = { 1'b1, k[2:0] };
	end
endfunction
reg [4:0] z_e_c;
reg [3:0] z_pg;
integer bi;
always @(*) begin
	z_e_c = 5'd0;
	for (bi = 0; bi < 4; bi = bi + 1) begin
		z_pg = pe8(bi == 3 ? { 1'b0, mZ[30:24] } : mZ[8 * bi +: 8]);
		if (z_pg[3]) z_e_c = { bi[1:0], z_pg[2:0] };
	end
end
wire [54:0] z_wide = { mZ[30:0], 24'd0 };   // Z[j] at j + 24: m = Z[e .. e - 23] = z_wide[e + 1 +: 24]
reg [4:0] z_e = 5'd0;
reg z_neg = 1'b0;
reg [23:0] z_m = 24'd0;

// the divider: r = floor(2^47 / m), m in [2^23, 2^24): remainder from 2^23, 25 quotient bits
reg [24:0] dv_rem = 25'd0;
reg [24:0] dv_q = 25'd0;
reg [4:0] dv_i = 5'd0;
wire [25:0] dv_sub = { 1'b0, dv_rem } - { 2'b00, z_m };

// one 33 x 25 multiply (S * r, T * r, (snext - s) * recip; b >= 0): set in clock c, the product ma_p in clock
// c + 5. Four partial products of one DSP each (a = ah * 2^17 + al, b = bh * 2^17 + bl), each registered, then
// summed in two registered steps (XST built a slow chain of DSPs from a plain a * b).
reg signed [32:0] ma_a = 0, ma_a1 = 0;
reg signed [25:0] ma_b = 0, ma_b1 = 0;
reg signed [58:0] ma_p = 0;
reg [33:0] mp_ll = 0;                     // al * bl
reg [24:0] mp_lh = 0;                     // al * bh
reg signed [33:0] mp_hl = 0;              // ah * bl
reg signed [24:0] mp_hh = 0;              // ah * bh
reg signed [58:0] mp_s1 = 0, mp_s2 = 0;
always @(posedge clk) begin
	ma_a1 <= ma_a;
	ma_b1 <= ma_b;
	mp_ll <= ma_a1[16:0] * ma_b1[16:0];
	mp_lh <= ma_a1[16:0] * ma_b1[24:17];
	mp_hl <= $signed(ma_a1[32:17]) * $signed({ 1'b0, ma_b1[16:0] });
	mp_hh <= $signed(ma_a1[32:17]) * $signed({ 1'b0, ma_b1[24:17] });
	mp_s1 <= $signed({ 25'd0, mp_ll }) + $signed({ 17'd0, mp_lh, 17'd0 });
	mp_s2 <= ($signed({ {8{mp_hl[33]}}, mp_hl, 17'd0 })) + ($signed({ mp_hh, 34'd0 }));
	ma_p <= mp_s1 + mp_s2;
end

// q = sat32(ma_p >> q_sh) (left for q_sh < 0); Z <= 0: by the sign of S / T. Two pipeline stages after the
// product (timing): ma_p2; stage A = the window, the significant bits, the sign (qa_*); stage B = q_val.
// The product of clock c + 5 (ma_p) gives q_val in clock c + 7, with the q_sh / q_sgn of clock c + 6.
reg signed [9:0] q_sh = 0;
reg q_sgn = 1'b0;
reg signed [58:0] ma_p2 = 0;
wire p_sign = ma_p2[58];
wire [127:0] q_ext = { {38{p_sign}}, ma_p2, 31'd0 };
wire [6:0] q_base = q_sh[6:0] + 7'd31;     // valid for -31 <= q_sh <= 62
reg [5:0] p_nb;                           // the significant bits of the product (bits 57..0 against the sign)
wire [63:0] p_x = { 6'd0, ma_p2[57:0] ^ {58{p_sign}} };
reg [3:0] p_pg;
integer pi;
always @(*) begin
	p_nb = 6'd0;
	for (pi = 0; pi < 8; pi = pi + 1) begin
		p_pg = pe8(p_x[8 * pi +: 8]);
		if (p_pg[3]) p_nb = { pi[2:0], p_pg[2:0] } + 6'd1;
	end
end
reg [38:0] qa_win = 39'd0;                // Ф5: the window by bytes (stage A), its bits 0..7 in stage B
reg [2:0] qa_fine = 3'd0;
reg [5:0] qa_nb = 6'd0;
reg qa_sign = 1'b0, qa_sgn = 1'b0;
reg signed [9:0] qa_sh = 0;
always @(posedge clk) begin
	ma_p2 <= ma_p;
	qa_win <= q_ext[{ q_base[6:3], 3'd0 } +: 39];
	qa_fine <= q_base[2:0];
	qa_nb <= p_nb;
	qa_sign <= p_sign;
	qa_sgn <= q_sgn;
	qa_sh <= q_sh;
end
reg signed [31:0] q_val;
always @(*) begin
	if (z_neg)
		q_val = qa_sgn ? 32'sh80000000 : 32'sh7FFFFFFF;
	else if (qa_sh >= 10'sd63)
		q_val = {32{qa_sign}};
	else if (qa_sh <= -10'sd32)
		q_val = qa_sign ? 32'sh80000000 : 32'sh7FFFFFFF;
	else if ($signed({ 4'd0, qa_nb }) > qa_sh + 10'sd31)
		q_val = qa_sign ? 32'sh80000000 : 32'sh7FFFFFFF;
	else
		q_val = qa_win[qa_fine +: 32];
end
reg signed [31:0] cl_q = 0, cl_adj = 0, cl_hi = 0;   // the clamp, one value per clock (s, then t)

// clamp as d_scan.c: the upper bound first
function signed [31:0] clampq(input signed [31:0] q, input signed [31:0] adj, input signed [31:0] lo, input signed [31:0] hi);
	reg signed [32:0] x;
	begin
		x = { q[31], q } + { adj[31], adj };
		if (x > $signed({ hi[31], hi })) clampq = hi;   // Ф7: signed (a concatenation is unsigned)
		else if (x < $signed({ lo[31], lo })) clampq = lo;
		else clampq = x[31:0];
	end
endfunction
wire signed [31:0] m_lo = m_init ? 32'sd0 : (m_left == 10'd0 ? 32'sd8 : 32'sd16);

// the reciprocal table of the last steps (65536 / n), as KG_QOPT
function [16:0] recip;
	input [3:0] n;
	case (n)
		4'd1: recip = 17'd65536; 4'd2: recip = 17'd32768; 4'd3: recip = 17'd21845; 4'd4: recip = 17'd16384;
		4'd5: recip = 17'd13107; 4'd6: recip = 17'd10923; 4'd7: recip = 17'd9362; 4'd8: recip = 17'd8192;
		4'd9: recip = 17'd7282; 4'd10: recip = 17'd6554; 4'd11: recip = 17'd5958; 4'd12: recip = 17'd5461;
		4'd13: recip = 17'd5041; 4'd14: recip = 17'd4681; 4'd15: recip = 17'd4369; default: recip = 17'd0;
	endcase
endfunction

wire [9:0] seg_n = m_left >= 10'd16 ? 10'd16 : m_left;

always @(posedge clk) begin
	pop <= 1'b0;
	if (d_take) d_valid <= 1'b0;
	if (tg_v[4]) case (tg_ch[9:8])     // a product of the accumulate pass
		2'd0: mS <= mS + ma_p[31:0];
		2'd1: mT <= mT + ma_p[31:0];
		default: mZ <= mZ + ma_p[31:0];
	endcase
	if (reset) begin
		ms <= M_IDLE;
		d_valid <= 1'b0;
	end else case (ms)
	M_IDLE: if (q_cnt != 3'd0 && !pop) ms <= M_POP;
	M_POP: begin
		pop <= 1'b1;
		m_mode <= q_head[91:90];
		if (!q_head[EW-1]) begin
			m_zn <= { 1'b0, q_head[89:81] } + 10'd1;
			m_zoff <= q_head[79:64];
			m_zizi <= q_head[63:32];
			m_zizs <= q_head[31:0];
			ms <= M_ZEMIT;
		end else begin
			m_left <= { 1'b0, q_head[89:81] } + 10'd1;
			m_v <= q_head[80:73];
			m_off <= { 8'd0, q_head[80:73] } * 16'd320 + { 7'd0, q_head[72:64] };
			mS <= 0; mT <= 0; mZ <= 0;
			ac_ch <= 2'd0; ac_grp <= 2'd0; ac_lg <= 1'b1; ac_iss <= 1'b1; ac_bu <= { 1'b0, q_head[72:64] };
			ms <= M_ACC;
		end
	end
	M_ZEMIT: if (!d_valid || d_take) begin
		d_valid <= 1'b1; d_z <= 1'b1; d_first <= 1'b1; d_last <= 1'b1; d_mode <= m_mode;
		d_n <= m_zn; d_off <= m_zoff; d_zacc <= m_zizi; d_zstep <= m_zizs; d_zsh <= 8'sd16;
		ms <= M_IDLE;
	end
	// S, T, Z += register * b (the span start: origin + u * grad_u + v * grad_v; a segment: + grad_u * (16 or
	// seg - 1)); done when all the products are in
	M_ACC: begin
		if (ac_iss) begin
			ma_a <= { pr_rd[31], pr_rd };
			ma_b <= ac_grp == 2'd0 ? 26'sd1 : ac_grp == 2'd1 ? { 16'd0, ac_bu } : { 18'd0, m_v };
			if (ac_ch == 2'd2) begin
				ac_ch <= 2'd0; ac_grp <= ac_grp + 2'd1;
				if (ac_grp == { 1'b0, ac_lg } + 2'd1) ac_iss <= 1'b0;
			end else
				ac_ch <= ac_ch + 2'd1;
		end else if (tg_v == 5'd0) begin
			if (ac_lg) begin
				mZ0 <= mZ;
				m_first <= 1'b1;
				m_init <= 1'b1;
			end
			ms <= M_NORM;
		end
	end
	// s, t of (mS, mT, mZ) with the bound m_lo: normalise Z, divide, multiply, shift, clamp
	M_NORM: begin
		z_neg <= mZ <= 0;
		z_e <= z_e_c;
		ms <= M_NORM2;
	end
	M_NORM2: begin
		z_m <= z_wide[{ 1'b0, z_e } + 6'd1 +: 24];
		dv_rem <= 25'h0800000;
		dv_q <= 25'd0;
		dv_i <= 5'd24;
		ms <= z_neg ? M_MS : M_DIV;
	end
	M_DIV: begin
		if (!dv_sub[25]) begin
			dv_rem <= { dv_sub[23:0], 1'b0 };
			dv_q[dv_i] <= 1'b1;
		end else
			dv_rem <= { dv_rem[23:0], 1'b0 };
		dv_i <= dv_i - 5'd1;
		if (dv_i == 5'd0) ms <= M_MS;
	end
	M_MS: begin                // S * r
		ma_a <= { mS[31], mS }; ma_b <= { 1'b0, dv_q };
		q_sh <= { {2{ks[7]}}, ks } - { {2{kz[7]}}, kz } + 10'sd8 + { 5'd0, z_e };
		q_sgn <= mS[31];
		ms <= M_MSW1;
	end
	M_MSW1: begin              // T * r
		ma_a <= { mT[31], mT }; ma_b <= { 1'b0, dv_q };
		ms <= M_MSW2;
	end
	M_MSW2: begin              // c + 2
		cl_adj <= pr_rd;           // sadjust
		ms <= M_MSW3;
	end
	M_MSW3: begin              // c + 3
		cl_hi <= pr_rd;            // bbextents
		ms <= M_MSW5;
	end
	M_MSW5: ms <= M_MSW6;      // c + 4
	M_MSW6: ms <= M_MSW4;      // c + 5: ma_p = S * r
	M_MSW4: begin              // c + 6: ma_p2 = S * r (stage A uses q_sh of S); the T shift from c + 7
		q_sh <= { {2{kt[7]}}, kt } - { {2{kz[7]}}, kz } + 10'sd8 + { 5'd0, z_e };
		q_sgn <= mT[31];
		ms <= M_MSQ;
	end
	M_MSQ: begin               // c + 7: q_val of S
		cl_q <= q_val;
		ms <= M_MTQ;
	end
	M_MTQ: begin               // c + 8: q_val of T; the clamp of S
		m_sn <= clampq(cl_q, cl_adj, m_lo, cl_hi);
		cl_q <= q_val; cl_adj <= pr_rd;   // tadjust
		ms <= M_MCL;
	end
	M_MCL: begin
		cl_hi <= pr_rd;            // bbextentt
		ms <= M_MCL2;
	end
	M_MCL2: begin              // the clamp of T
		m_tn <= clampq(cl_q, cl_adj, m_lo, cl_hi);
		ms <= M_AFTER;
	end
	M_AFTER: begin
		if (m_init) begin
			m_s <= m_sn; m_t <= m_tn;
			m_init <= 1'b0;
			ms <= M_SEG;
		end else if (m_left != 10'd0) begin
			m_ss <= (m_sn - m_s) >>> 4;
			m_ts <= (m_tn - m_t) >>> 4;
			ms <= M_EMIT;
		end else
			ms <= (m_seg > 5'd1) ? M_STEP : M_EMIT;
	end
	// the next segment: S, T, Z at its far end
	M_SEG: begin
		m_seg <= seg_n[4:0];
		m_left <= m_left - seg_n;
		ac_ch <= 2'd0; ac_grp <= 2'd1; ac_lg <= 1'b0; ac_iss <= 1'b1;
		ac_bu <= m_left != seg_n ? 10'd16 : seg_n - 10'd1;
		ms <= M_ACC;
	end
	// the last steps: ((snext - s) * recip(seg - 1)) >> 16
	M_STEP: begin
		ma_a <= { m_sn[31], m_sn } - { m_s[31], m_s }; ma_b <= { 9'd0, recip(m_seg[3:0] - 4'd1) };
		ms <= M_SW1;
	end
	M_SW1: begin
		ma_a <= { m_tn[31], m_tn } - { m_t[31], m_t }; ma_b <= { 9'd0, recip(m_seg[3:0] - 4'd1) };
		ms <= M_SW2;
	end
	M_SW2: ms <= M_SW3;
	M_SW3: ms <= M_SW4;
	M_SW4: ms <= M_SWQ;       // c + 5: ma_p = (snext - s) * recip
	M_SWQ: begin
		m_ss <= ma_p[47:16];
		ms <= M_SWQ2;
	end
	M_SWQ2: begin
		m_ts <= ma_p[47:16];
		ms <= M_EMIT;
	end
	M_EMIT: if (!d_valid || d_take) begin
		d_valid <= 1'b1; d_z <= 1'b0; d_first <= m_first; d_last <= m_left == 10'd0; d_mode <= m_mode;
		d_n <= { 5'd0, m_seg }; d_off <= m_off; d_tex <= tex; d_tw <= tw;
		d_s <= m_s; d_t <= m_t; d_ss <= m_ss; d_ts <= m_ts;
		d_zacc <= mZ0; d_zstep <= pr_rd; d_zsh <= kz - 8'sd15;   // zu
		m_first <= 1'b0;
		m_off <= m_off + { 11'd0, m_seg };
		m_s <= m_sn; m_t <= m_tn;
		ms <= (m_left == 10'd0) ? M_IDLE : M_SEG;
	end
	default: ms <= M_IDLE;
	endcase
end

// ------------------------------------------------------------------ the pixel part
// Ф7: a tested span (d_mode 1: the mask, 2: the z) checks the mask halfword of the pixel first (P_MLD; if it is
// not the one held: written back if it changed, P_MWB, then read, P_MRD), mode 2 reads the z (P_ZRD); a hidden
// pixel only steps (its pending even neighbour written alone, P_FLUSH); a drawn one sets its mask bit (the texel
// address in the same clock). A z span with polymode marks the mask too; a mask clear (mode 3) writes 0 to the
// mask halfwords. The changed mask halfword is written back at the end of a span (P_ENDWB). The step to the next
// pixel is done in the clock that ends the pixel (`P_STEP).
localparam [3:0] P_IDLE = 4'd0, P_ADDR = 4'd1, P_TEX = 4'd2, P_PIX = 4'd3, P_Z = 4'd4, P_MLD = 4'd5, P_MWB = 4'd6,
	P_MRD = 4'd7, P_ZRD = 4'd8, P_FLUSH = 4'd10, P_ENDWB = 4'd12;
reg [3:0] ps = P_IDLE;
reg p_z = 1'b0, p_last = 1'b0;
reg [1:0] p_mode = 2'd0;
reg [9:0] p_n = 10'd0;
reg [15:0] p_off = 16'd0;
reg signed [31:0] p_s = 0, p_t = 0, p_ss = 0, p_ts = 0;
reg [31:0] p_zacc = 32'd0, p_zstep = 32'd0;
reg signed [7:0] p_zsh = 0;
reg [21:0] p_a = 22'd0;                   // the texel byte address
reg [7:0] p_byte = 8'd0;
reg lt_valid = 1'b0;                      // the halfword read last
reg [20:0] lt_ha = 21'd0;
reg [15:0] lt_hw = 16'd0;
reg pend = 1'b0;                          // an even pixel waits for its odd neighbour
reg [7:0] pend_lo = 8'd0;
reg mh_valid = 1'b0, mh_dirty = 1'b0;     // Ф7: the mask halfword held
reg [11:0] mh_idx = 12'd0;
reg [15:0] mh_val = 16'd0;

// the z halfword: zacc >> sh (arithmetic; sh >= 32: the sign), sh < 0: zacc << -sh (-sh >= 32: 0)
function [15:0] zval;
	input [31:0] acc;
	input signed [7:0] sh;
	reg [31:0] x;
	begin
		if (sh >= 8'sd32) zval = acc[31] ? 16'hFFFF : 16'h0000;
		else if (sh >= 8'sd0) begin x = $signed(acc) >>> sh[4:0]; zval = x[15:0]; end
		else if (sh <= -8'sd32) zval = 16'h0000;
		else begin x = acc << (-sh); zval = x[15:0]; end
	end
endfunction

reg [21:0] p_tex = 22'd0;
reg [11:0] p_tw = 12'd0;
wire [27:0] p_row = p_t[31:16] * p_tw;    // (t >> 16) * tw (a DSP)
wire [21:0] p_a_c = p_tex + { 6'd0, p_s[31:16] } + p_row[21:0];
wire [15:0] p_zv0 = zval(p_zacc, p_zsh);
wire [15:0] p_zv = (p_mode != 2'd0 && !p_z && (p_zv0 == 16'd0 || p_zv0[15])) ? 16'd1 : p_zv0;   // a tested span: >= 1
wire p_mtest = p_mode == 2'd1 || p_mode == 2'd2 || (p_z && p_mode == 2'd0 && polymode);   // the mask is used
wire [15:0] p_mbit = 16'd1 << p_off[3:0];
wire p_mhit = mh_valid && mh_idx == p_off[15:4];

// the step to the next pixel (or the end of the descriptor: the changed mask halfword written back at the end of
// a span); dirty = mh_dirty after this clock
`define P_STEP(dirty) \
	p_s <= p_s + p_ss; p_t <= p_t + p_ts; \
	p_zacc <= p_zacc + p_zstep; \
	p_off <= p_off + 16'd1; \
	p_n <= p_n - 10'd1; \
	if (p_n == 10'd1) ps <= (p_last && (dirty)) ? P_ENDWB : P_IDLE; \
	else ps <= p_mtest ? P_MLD : p_z ? P_Z : P_ADDR;

always @(posedge clk) begin
	d_take <= 1'b0;
	if (!busy) lt_valid <= 1'b0;          // the surface cache may change while the drawer is idle
	if (reset) begin
		ps <= P_IDLE;
		sr_req <= 1'b0;
		pend <= 1'b0;
		mh_valid <= 1'b0; mh_dirty <= 1'b0;
	end else case (ps)
	P_IDLE: if (d_valid && !d_take) begin
		d_take <= 1'b1;
		p_z <= d_z; p_last <= d_last; p_n <= d_n; p_off <= d_off; p_mode <= d_mode; p_tex <= d_tex; p_tw <= d_tw;
		p_s <= d_s; p_t <= d_t; p_ss <= d_ss; p_ts <= d_ts;
		if (d_first) p_zacc <= d_zacc;
		p_zstep <= d_zstep; p_zsh <= d_zsh;
		if (d_z && d_mode == 2'd3) mh_valid <= 1'b0;   // a mask clear: the held halfword is stale
		ps <= (d_mode == 2'd1 || d_mode == 2'd2 || (d_z && d_mode == 2'd0 && polymode)) ? P_MLD :
		      d_z ? P_Z : P_ADDR;
	end
	P_MLD: begin
		if (!p_mhit)
			ps <= (mh_valid && mh_dirty) ? P_MWB : P_MRD;
		else if (p_z) begin                // a z span marks its pixel
			mh_val <= mh_val | p_mbit; mh_dirty <= 1'b1;
			ps <= P_Z;
		end else if (p_mode == 2'd2)
			ps <= P_ZRD;
		else if (mh_val[p_off[3:0]]) begin // covered: hidden
			if (pend) ps <= P_FLUSH;
			else begin `P_STEP(mh_dirty) end
		end else begin
			mh_val <= mh_val | p_mbit; mh_dirty <= 1'b1;
			p_a <= p_a_c;
			ps <= P_TEX;
		end
	end
	P_MWB, P_ENDWB: begin
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b1; sr_sel <= 2'b11;
			sr_adr <= MASK[21:1] + { 9'd0, mh_idx };
			sr_dat <= mh_val;
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			mh_dirty <= 1'b0;
			ps <= ps == P_ENDWB ? P_IDLE : P_MRD;
		end
	end
	P_MRD: begin
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b0;
			sr_adr <= MASK[21:1] + { 9'd0, p_off[15:4] };
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			mh_valid <= 1'b1; mh_dirty <= 1'b0; mh_idx <= p_off[15:4]; mh_val <= sr_rdat;
			ps <= P_MLD;
		end
	end
	P_ZRD: begin
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b0;
			sr_adr <= ZB[21:1] + { 5'd0, p_off };
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			if ($signed(p_zv) < $signed(sr_rdat)) begin   // behind: hidden
				if (pend) ps <= P_FLUSH;
				else begin `P_STEP(mh_dirty) end
			end else begin
				mh_val <= mh_val | p_mbit; mh_dirty <= 1'b1;
				p_a <= p_a_c;
				ps <= P_TEX;
			end
		end
	end
	P_FLUSH: begin                       // the even neighbour alone (this odd pixel is hidden)
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b1; sr_sel <= 2'b01;
			sr_adr <= FB[21:1] + { 5'd0, p_off[15:1] };
			sr_dat <= { 8'd0, pend_lo };
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			pend <= 1'b0;
			`P_STEP(mh_dirty)
		end
	end
	P_ADDR: begin
		p_a <= p_a_c;
		ps <= P_TEX;
	end
	P_TEX: begin
		if (lt_valid && p_a[21:1] == lt_ha && !sr_req) begin
			p_byte <= p_a[0] ? lt_hw[15:8] : lt_hw[7:0];
			ps <= P_PIX;
		end else if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b0; sr_adr <= p_a[21:1];
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			lt_valid <= 1'b1; lt_ha <= p_a[21:1]; lt_hw <= sr_rdat;
			p_byte <= p_a[0] ? sr_rdat[15:8] : sr_rdat[7:0];
			ps <= P_PIX;
		end
	end
	P_PIX: begin
		if (!p_off[0] && (p_n != 10'd1 || !p_last) && !sr_req) begin
			pend <= 1'b1; pend_lo <= p_byte;          // an even pixel: the odd one follows in this span
			ps <= P_Z;
		end else if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b1;
			sr_adr <= FB[21:1] + { 5'd0, p_off[15:1] };
			if (p_off[0]) begin
				sr_sel <= pend ? 2'b11 : 2'b10;
				sr_dat <= { p_byte, pend_lo };
			end else begin
				sr_sel <= 2'b01;
				sr_dat <= { 8'd0, p_byte };
			end
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			pend <= 1'b0;
			ps <= P_Z;
		end
	end
	P_Z: begin
		if (!sr_req) begin
			sr_req <= 1'b1; sr_we <= 1'b1;
			sr_adr <= (p_z && p_mode == 2'd3) ? MASK[21:1] + { 5'd0, p_off } : ZB[21:1] + { 5'd0, p_off };
			sr_sel <= 2'b11;
			sr_dat <= (p_z && p_mode == 2'd3) ? 16'd0 : p_zv;
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			`P_STEP(mh_dirty)
		end
	end
	default: ps <= P_IDLE;
	endcase
end
`undef P_STEP

assign core_idle = q_cnt == 3'd0 && ms == M_IDLE && !d_valid && ps == P_IDLE;
assign busy = cf_cnt != 10'd0 || rs != R_IDLE || r_start || q_push || !core_idle;
assign io_hold = io_sel && cf_full;

endmodule
