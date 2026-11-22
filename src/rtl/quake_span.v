`timescale 1ns / 1ps
`default_nettype none
/*
 quake_span.v (karabas, Quake Ф3: the span drawer; Ф3е: with the perspective)

 The inner work of D_DrawSpans8 + D_DrawZSpans (d_scan.c, KG_HWSPAN + KG_HWPERSP) in the FPGA. The texture (the
 surface cache block), the frame and the z-buffer are in the SRAM, reached through the drawer port of doom_sram
 (a halfword read or a lane write, one at a time). Functional model, bit for bit: src/sim/qspan_model.h.

 Registers (MMIO 0x100..0x17c, src/fw/mmio.h; io_off = (offset - 0x100) / 4):
   00 TEX  SRAM byte address of the texture (21..0)        01 TW   its row bytes (cachewidth, 11..0)
   06 IZI, 07 IZISTEP: 1/z 16.16 of a z span (z = izi >> 16)
   09 ZSPAN  = { count - 1 (24..16), pixel offset (15..0) }: queues count z halfwords into the z-buffer
   16..24 SO SU SV TO TU TV ZO ZU ZV: the surface, S = sdivz * 2^ks, T = tdivz * 2^kt, Z = zi * 2^kz, each
          origin + u * grad_u + v * grad_v at screen pixel (u, v)
   25 K = { kz (23..16), kt (15..8), ks (7..0) } signed      26 SADJ, 27 TADJ (sadjust, tadjust)
   28 BBS, 29 BBT (bbextents, bbextentt)
   30 PSPAN = { count - 1 (25..17), v (16..9), u (8..0) }: queues a perspective span (texels and z)
   (the Ф3 registers S / T / SSTEP / TSTEP / SPAN, 02..05 / 08, are gone: the firmware with KG_HWPERSP)
 io_hold (to doom_soc, for the presented write io_sel / io_off): ZSPAN / PSPAN while the queue is full; TEX, TW
 and the surface registers while the drawer has work (the queued spans read them; measured cost ~0).
 busy: work queued or running (doom_soc: the CPU waits for the frame / z-buffer SRAM and the frame copy).

 The math part takes the commands. A z span goes to the pixel part as one descriptor. A perspective span: S, T,
 Z at (u, v) (three 32 x 10 multiplies), then s, t at the span start and at the far end of each 16-pixel
 segment (the last one: of its last pixel): Z = m * 2^(e - 23), r = floor(2^47 / m) (a 25-step divider),
 q = sat32((S * r) >> (ks - kz + 8 + e)) (a left shift for a negative amount), s = q + sadjust clamped as d_scan.c
 (the upper bound first); the steps (>> 4, or the reciprocal table on the last segment); one descriptor per
 segment. The pixel part draws a descriptor while the math part works on the next one: per pixel the texel
 (a halfword read unless it is the halfword read last), the pixel (two neighbours of a span in one halfword
 write), the z halfword (Z >> (kz - 15), or izi >> 16 of a z span).
*/
module quake_span #(
	parameter [21:0] FB = 22'h3f0000,     // the work frame (byte address in the SRAM)
	parameter [21:0] ZB = 22'h3b0000      // the z-buffer
) (
	input wire clk,
	input wire reset,

	input wire io_sel,                    // a write to 0x100..0x17c is presented (doom_soc)
	input wire [4:0] io_off,
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

// ------------------------------------------------------------------ registers
reg [21:0] tex = 22'd0;
reg [11:0] tw = 12'd0;
reg [31:0] r_izi = 32'd0, r_izs = 32'd0;
reg signed [31:0] so = 0, su = 0, sv = 0, to = 0, tu = 0, tv = 0, zo = 0, zu = 0, zv = 0;
reg signed [7:0] ks = 0, kt = 0, kz = 0;
reg signed [31:0] sadj = 0, tadj = 0, bbs = 0, bbt = 0;

wire is_push = io_off == 5'd9 || io_off == 5'd30;
wire is_param = io_off == 5'd0 || io_off == 5'd1 || (io_off >= 5'd16 && io_off <= 5'd29);

always @(posedge clk) begin
	if (io_we) case (io_off)
		5'd0: tex <= io_wd[21:0];
		5'd1: tw <= io_wd[11:0];
		5'd6: r_izi <= io_wd;
		5'd7: r_izs <= io_wd;
		5'd16: so <= io_wd;
		5'd17: su <= io_wd;
		5'd18: sv <= io_wd;
		5'd19: to <= io_wd;
		5'd20: tu <= io_wd;
		5'd21: tv <= io_wd;
		5'd22: zo <= io_wd;
		5'd23: zu <= io_wd;
		5'd24: zv <= io_wd;
		5'd25: begin ks <= io_wd[7:0]; kt <= io_wd[15:8]; kz <= io_wd[23:16]; end
		5'd26: sadj <= io_wd;
		5'd27: tadj <= io_wd;
		5'd28: bbs <= io_wd;
		5'd29: bbt <= io_wd;
		default: ;
	endcase
end

// ------------------------------------------------------------------ the queue (distributed RAM, one write per clock)
localparam QN = 4;
localparam EW = 1 + 9 + 17 + 64;          // { p, n - 1, uv / off, izi, izistep }
reg [EW-1:0] q_mem [0:QN-1];
reg [1:0] q_wp = 2'd0, q_rp = 2'd0;
reg [2:0] q_cnt = 3'd0;
wire q_full = q_cnt == QN;
wire push = io_we && is_push && !q_full;
wire [EW-1:0] q_in = io_off == 5'd30 ? { 1'b1, io_wd[25:17], io_wd[16:0], 64'd0 }
                                     : { 1'b0, io_wd[24:16], 1'b0, io_wd[15:0], r_izi, r_izs };
reg pop = 1'b0;
wire [EW-1:0] q_head = q_mem[q_rp];

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
end

// ------------------------------------------------------------------ the descriptor (math -> pixel)
reg d_valid = 1'b0;
reg d_z = 1'b0;                           // a z span (no texels)
reg d_first = 1'b0, d_last = 1'b0;        // first / last segment of a perspective span
reg [9:0] d_n = 10'd0;                    // pixels (1..512)
reg [15:0] d_off = 16'd0;
reg signed [31:0] d_s = 0, d_t = 0, d_ss = 0, d_ts = 0;
reg [31:0] d_zacc = 32'd0, d_zstep = 32'd0;
reg signed [7:0] d_zsh = 0;
reg d_take = 1'b0;                        // the pixel part takes the descriptor (a pulse)

// ------------------------------------------------------------------ the math part
localparam [4:0] M_IDLE = 5'd0, M_POP = 5'd1, M_ZEMIT = 5'd2, M_INIT1 = 5'd3, M_INIT2 = 5'd4, M_INIT3 = 5'd5,
	M_NORM = 5'd6, M_DIV = 5'd7, M_MS = 5'd8, M_MSW1 = 5'd9, M_MSW2 = 5'd10, M_MSQ = 5'd11, M_MTQ = 5'd12,
	M_MCL = 5'd13, M_AFTER = 5'd14, M_SEG = 5'd15, M_LM1 = 5'd16, M_LM2 = 5'd17, M_STEP = 5'd18, M_SW1 = 5'd19,
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
reg [9:0] m_zn = 10'd0;                   // a z span
reg [15:0] m_zoff = 16'd0;
reg [31:0] m_zizi = 32'd0, m_zizs = 32'd0;

// three 32 x 10 multiplies (S, T, Z grads by u, v, or by seg - 1): one clock
reg signed [31:0] mu_a0 = 0, mu_a1 = 0, mu_a2 = 0;
reg [9:0] mu_b = 10'd0;
reg signed [42:0] mu_p0 = 0, mu_p1 = 0, mu_p2 = 0;
always @(posedge clk) begin
	mu_p0 <= mu_a0 * $signed({ 1'b0, mu_b });
	mu_p1 <= mu_a1 * $signed({ 1'b0, mu_b });
	mu_p2 <= mu_a2 * $signed({ 1'b0, mu_b });
end

// Z normalised: e = the top bit, m = its 24 bits from there
reg [4:0] z_e_c;
integer bi;
always @(*) begin
	z_e_c = 5'd0;
	for (bi = 0; bi < 31; bi = bi + 1)
		if (mZ[bi]) z_e_c = bi;
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
wire [124:0] q_ext = { {35{p_sign}}, ma_p2, 31'd0 };
wire [6:0] q_base = q_sh[6:0] + 7'd31;     // valid for -31 <= q_sh <= 62
reg [5:0] p_nb;                           // the significant bits of the product
integer pi;
always @(*) begin
	p_nb = 6'd0;
	for (pi = 0; pi < 58; pi = pi + 1)
		if (ma_p2[pi] != p_sign) p_nb = pi + 1;
end
reg [31:0] qa_win = 32'd0;
reg [5:0] qa_nb = 6'd0;
reg qa_sign = 1'b0, qa_sgn = 1'b0;
reg signed [9:0] qa_sh = 0;
always @(posedge clk) begin
	ma_p2 <= ma_p;
	qa_win <= q_ext[q_base +: 32];
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
		q_val = qa_win;
end
reg signed [31:0] cl_q = 0, cl_adj = 0, cl_hi = 0;   // the clamp, one value per clock (s, then t)

// clamp as d_scan.c: the upper bound first
function signed [31:0] clampq(input signed [31:0] q, input signed [31:0] adj, input signed [31:0] lo, input signed [31:0] hi);
	reg signed [32:0] x;
	begin
		x = { q[31], q } + { adj[31], adj };
		if (x > { hi[31], hi }) clampq = hi;
		else if (x < { lo[31], lo }) clampq = lo;
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
	if (reset) begin
		ms <= M_IDLE;
		d_valid <= 1'b0;
	end else case (ms)
	M_IDLE: if (q_cnt != 3'd0 && !pop) ms <= M_POP;
	M_POP: begin
		pop <= 1'b1;
		if (!q_head[EW-1]) begin
			m_zn <= { 1'b0, q_head[EW-2:EW-10] } + 10'd1;
			m_zoff <= q_head[79:64];
			m_zizi <= q_head[63:32];
			m_zizs <= q_head[31:0];
			ms <= M_ZEMIT;
		end else begin
			m_left <= { 1'b0, q_head[EW-2:EW-10] } + 10'd1;
			m_v <= q_head[80:73];
			m_off <= { 8'd0, q_head[80:73] } * 16'd320 + { 7'd0, q_head[72:64] };
			mu_a0 <= su; mu_a1 <= tu; mu_a2 <= zu; mu_b <= { 1'b0, q_head[72:64] };
			ms <= M_INIT1;
		end
	end
	M_ZEMIT: if (!d_valid || d_take) begin
		d_valid <= 1'b1; d_z <= 1'b1; d_first <= 1'b1; d_last <= 1'b1;
		d_n <= m_zn; d_off <= m_zoff; d_zacc <= m_zizi; d_zstep <= m_zizs; d_zsh <= 8'sd16;
		ms <= M_IDLE;
	end
	// S, T, Z = origin + u * grad_u + v * grad_v (the u products compute in INIT1, the v ones in INIT2)
	M_INIT1: begin
		mu_a0 <= sv; mu_a1 <= tv; mu_a2 <= zv; mu_b <= { 2'b00, m_v };
		ms <= M_INIT2;
	end
	M_INIT2: begin
		mS <= so + mu_p0[31:0]; mT <= to + mu_p1[31:0]; mZ <= zo + mu_p2[31:0];
		ms <= M_INIT3;
	end
	M_INIT3: begin
		mS <= mS + mu_p0[31:0]; mT <= mT + mu_p1[31:0]; mZ <= mZ + mu_p2[31:0];
		mZ0 <= mZ + mu_p2[31:0];
		m_first <= 1'b1;
		m_init <= 1'b1;
		ms <= M_NORM;
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
	M_MSW2: ms <= M_MSW3;      // c + 2
	M_MSW3: ms <= M_MSW5;      // c + 3
	M_MSW5: ms <= M_MSW6;      // c + 4
	M_MSW6: ms <= M_MSW4;      // c + 5: ma_p = S * r
	M_MSW4: begin              // c + 6: ma_p2 = S * r (stage A uses q_sh of S); the T shift from c + 7
		q_sh <= { {2{kt[7]}}, kt } - { {2{kz[7]}}, kz } + 10'sd8 + { 5'd0, z_e };
		q_sgn <= mT[31];
		ms <= M_MSQ;
	end
	M_MSQ: begin               // c + 7: q_val of S
		cl_q <= q_val; cl_adj <= sadj; cl_hi <= bbs;
		ms <= M_MTQ;
	end
	M_MTQ: begin               // c + 8: q_val of T; the clamp of S
		m_sn <= clampq(cl_q, cl_adj, m_lo, cl_hi);
		cl_q <= q_val; cl_adj <= tadj; cl_hi <= bbt;
		ms <= M_MCL;
	end
	M_MCL: begin               // the clamp of T
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
		if (m_left != seg_n) begin
			mS <= mS + (su <<< 4); mT <= mT + (tu <<< 4); mZ <= mZ + (zu <<< 4);
			ms <= M_NORM;
		end else begin
			mu_a0 <= su; mu_a1 <= tu; mu_a2 <= zu; mu_b <= seg_n - 10'd1;
			ms <= M_LM1;
		end
	end
	M_LM1: ms <= M_LM2;       // grad * (seg - 1) computing
	M_LM2: begin
		mS <= mS + mu_p0[31:0]; mT <= mT + mu_p1[31:0]; mZ <= mZ + mu_p2[31:0];
		ms <= M_NORM;
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
		d_valid <= 1'b1; d_z <= 1'b0; d_first <= m_first; d_last <= m_left == 10'd0;
		d_n <= { 5'd0, m_seg }; d_off <= m_off;
		d_s <= m_s; d_t <= m_t; d_ss <= m_ss; d_ts <= m_ts;
		d_zacc <= mZ0; d_zstep <= zu; d_zsh <= kz - 8'sd15;
		m_first <= 1'b0;
		m_off <= m_off + { 11'd0, m_seg };
		m_s <= m_sn; m_t <= m_tn;
		ms <= (m_left == 10'd0) ? M_IDLE : M_SEG;
	end
	default: ms <= M_IDLE;
	endcase
end

// ------------------------------------------------------------------ the pixel part
localparam [2:0] P_IDLE = 3'd0, P_ADDR = 3'd1, P_TEX = 3'd2, P_PIX = 3'd3, P_Z = 3'd4;
reg [2:0] ps = P_IDLE;
reg p_z = 1'b0, p_last = 1'b0;
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

wire [27:0] p_row = p_t[31:16] * tw;      // (t >> 16) * tw (a DSP)

always @(posedge clk) begin
	d_take <= 1'b0;
	if (!busy) lt_valid <= 1'b0;          // the surface cache may change while the drawer is idle
	if (reset) begin
		ps <= P_IDLE;
		sr_req <= 1'b0;
		pend <= 1'b0;
	end else case (ps)
	P_IDLE: if (d_valid && !d_take) begin
		d_take <= 1'b1;
		p_z <= d_z; p_last <= d_last; p_n <= d_n; p_off <= d_off;
		p_s <= d_s; p_t <= d_t; p_ss <= d_ss; p_ts <= d_ts;
		if (d_first) p_zacc <= d_zacc;
		p_zstep <= d_zstep; p_zsh <= d_zsh;
		ps <= d_z ? P_Z : P_ADDR;
	end
	P_ADDR: begin
		p_a <= tex + { 6'd0, p_s[31:16] } + p_row[21:0];
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
			sr_adr <= ZB[21:1] + { 5'd0, p_off };
			sr_sel <= 2'b11;
			sr_dat <= zval(p_zacc, p_zsh);
		end else if (sr_ack) begin
			sr_req <= 1'b0;
			p_s <= p_s + p_ss; p_t <= p_t + p_ts;
			p_zacc <= p_zacc + p_zstep;
			p_off <= p_off + 16'd1;
			p_n <= p_n - 10'd1;
			if (p_n == 10'd1) ps <= P_IDLE;
			else ps <= p_z ? P_Z : P_ADDR;
		end
	end
	default: ps <= P_IDLE;
	endcase
end

assign busy = q_cnt != 3'd0 || ms != M_IDLE || d_valid || ps != P_IDLE;
assign io_hold = io_sel && ((is_push && q_full) || (is_param && busy));

endmodule
