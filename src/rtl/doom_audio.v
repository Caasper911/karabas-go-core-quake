`timescale 1ns / 1ps
`default_nettype none
/*
 doom_audio.v

 The sound of the Doom core (docs/PLAN.md Ф2): the sound effects mixed by the CPU, an OPL3 for the
 music, the MIDI out to the SAM2695 synth of the board and its sound back through the PCM1808 ADC.

 CPU side (clk, 75 MHz), MMIO word registers (offsets of doom_soc, mmio.h):
   0x60 W  OPL_ADDR  bits 8..0: { bank, register } of the next OPL_DATA write
   0x64 W  OPL_DATA  bits 7..0: the value -> opl3_fm (opl3_fpga, LGPL-3: the FM part of the OPL4 of
                     core-tsconf, as core-zxuno), one sample per 1 / 49716 s (the rate of the YMF262)
   0x68 W  MIDI      bits 7..0: a byte to MIDI_TX (kg_midi_tx, a fifo of 64 bytes, 31250 baud)
   0x70 R  SFX_POS   the ring index the output reads now (0..2047)
   0x2000..0x3ffc W  SFX ring: entry (a - 0x2000) / 4 = { right[15:0], left[15:0] } (signed)
 The CPU keeps the ring ahead of SFX_POS (src/fw/i_kgsound.c); the output reads it at 11025 Hz.

 Output side (clk_a, 25 MHz): the ring at 11025 Hz (held), the OPL3 samples (held), the ADC (the
 PCM1808 in slave mode: ADC_CLK = clk_a = 256 fs, fs 97.7 kHz from i2s_transceiver), mixed and
 saturated: snd_l / snd_r for the DAC and HDMI (sampled by the top). Levels: SFX x1, OPL x1 (opl3_fm
 out / 4 + / 8 as core-zxuno), ADC x adc_gain / 2 (the SAM2695 is quiet: x2 by default).
*/
module doom_audio #(
	parameter CLK_HZ = 75000000,          // clk (kg_midi_tx: 31250 baud)
	parameter [32:0] OPL_STEP = 33'd2847048, // 49716 * 2^32 / clk: 2847048 at 75 MHz, 2684359 at 875/11 MHz
	parameter WITH_OPL = 1                // Quake (karabas): 0 = no OPL3 (its registers do nothing, silent)
) (
	input wire clk,
	input wire reset,

	input wire io_we,
	input wire [13:0] io_off,             // byte offset in the MMIO page
	input wire [31:0] io_wd,
	output reg [31:0] io_rd = 32'd0,      // the value of io_off (combinational from registers)

	output wire midi_tx,

	input wire clk_a,
	output wire adc_clk_en,               // (the top drives ADC_CLK from clk_a)
	output wire adc_bck,
	output wire adc_lrck,
	input wire adc_dout,
	input wire [2:0] adc_gain,            // x adc_gain / 2
	output reg signed [15:0] snd_l = 16'sd0,
	output reg signed [15:0] snd_r = 16'sd0
);

assign adc_clk_en = 1'b1;

// ------------------------------------------------------------------ OPL3 (clk)

reg [8:0] opl_addr = 9'd0;
reg opl_wtgl = 1'b0, opl_ttgl = 1'b0;
reg [16:0] opl_word = 17'd0;
reg [31:0] opl_ph = 32'd0;
wire [32:0] opl_ph_n = { 1'b0, opl_ph } + OPL_STEP;      // 49716 Hz
wire signed [19:0] fm_l, fm_r;
reg signed [15:0] opl_l = 16'sd0, opl_r = 16'sd0;

function signed [15:0] sat16(input signed [19:0] v);
	sat16 = (v > 20'sd32767) ? 16'sh7FFF : (v < -20'sd32768) ? 16'sh8000 : v[15:0];
endfunction

always @(posedge clk) begin
	if (io_we && io_off == 14'h0060) opl_addr <= io_wd[8:0];
	if (io_we && io_off == 14'h0064) begin
		opl_word <= { opl_addr, io_wd[7:0] };
		opl_wtgl <= ~opl_wtgl;
	end
	opl_ph <= opl_ph_n[31:0];
	if (opl_ph_n[32]) begin
		opl_ttgl <= ~opl_ttgl;
		opl_l <= sat16((fm_l >>> 2) + (fm_l >>> 3));
		opl_r <= sat16((fm_r >>> 2) + (fm_r >>> 3));
	end
end

generate if (WITH_OPL) begin : g_opl
opl3_fm fm (
	.clk(clk),
	.reset_a(reset),
	.wr_tgl(opl_wtgl),
	.wr_word(opl_word),
	.tick_tgl(opl_ttgl),
	.out_l(fm_l),
	.out_r(fm_r)
);
end else begin : g_noopl
assign fm_l = 20'sd0;
assign fm_r = 20'sd0;
end endgenerate

// ------------------------------------------------------------------ MIDI (clk)

kg_midi_tx #(.CLK_HZ(CLK_HZ)) midi (
	.clk(clk),
	.data(io_wd[7:0]),
	.wr(io_we && io_off == 14'h0068),
	.overflow(),
	.tx(midi_tx)
);

// ------------------------------------------------------------------ SFX ring (clk -> clk_a)

wire [31:0] ring_q;
reg [10:0] rd_idx = 11'd0;                // clk_a
kg_sdpram #(.DW(32), .AW(11)) ring (
	.clka(clk), .wea(io_we && io_off[13] == 1'b1), .addra(io_off[12:2]), .dina(io_wd),
	.clkb(clk_a), .enb(1'b1), .addrb(rd_idx), .doutb(ring_q)
);

// the read index to clk as a Gray code
reg [10:0] rd_gray = 11'd0;               // clk_a
wire [10:0] rd_gray_s;
kg_sync #(.W(11)) sync_pos (.clk(clk), .d(rd_gray), .q(rd_gray_s));
reg [10:0] pos_bin;
integer k;
always @(*) begin
	pos_bin[10] = rd_gray_s[10];
	for (k = 9; k >= 0; k = k - 1) pos_bin[k] = pos_bin[k + 1] ^ rd_gray_s[k];
end

always @(*) begin
	case (io_off)
		14'h0070: io_rd = { 21'd0, pos_bin };
		default: io_rd = 32'd0;
	endcase
end

// ------------------------------------------------------------------ output (clk_a)

// 11025 Hz: + 1894081 / 2^32 per clock of 25 MHz
reg [31:0] sfx_ph = 32'd0;
wire [32:0] sfx_ph_n = { 1'b0, sfx_ph } + 33'd1894081;
reg signed [15:0] sfx_l = 16'sd0, sfx_r = 16'sd0;
reg sfx_take = 1'b0;
always @(posedge clk_a) begin
	sfx_ph <= sfx_ph_n[31:0];
	sfx_take <= 1'b0;
	if (sfx_ph_n[32]) begin
		rd_idx <= rd_idx + 11'd1;
		sfx_take <= 1'b1;               // ring_q of the old rd_idx is out a clock later
	end
	if (sfx_take) begin
		sfx_l <= ring_q[15:0];
		sfx_r <= ring_q[31:16];
		rd_gray <= rd_idx ^ (rd_idx >> 1);
	end
end

// the OPL3 samples into clk_a: they change once per 1508 clocks of clk, a double register is enough
// for a value that is held (a sample taken in its change can mix old and new bits: two of them)
reg signed [15:0] opl_l1 = 16'sd0, opl_r1 = 16'sd0, opl_l2 = 16'sd0, opl_r2 = 16'sd0;
always @(posedge clk_a) begin
	opl_l1 <= opl_l; opl_r1 <= opl_r;
	if (opl_l1 == opl_l) opl_l2 <= opl_l1;
	if (opl_r1 == opl_r) opl_r2 <= opl_r1;
end

wire [23:0] adc_l24, adc_r24;
i2s_transceiver adc (
	.reset_n(~reset),
	.mclk(clk_a),
	.sclk(adc_bck),
	.ws(adc_lrck),
	.sd_tx(),
	.sd_rx(adc_dout),
	.l_data_tx(24'b0),
	.r_data_tx(24'b0),
	.l_data_rx(adc_l24),
	.r_data_rx(adc_r24)
);
wire signed [15:0] adc_l = adc_l24[23:8];
wire signed [15:0] adc_r = adc_r24[23:8];

wire signed [20:0] adc_lg = (adc_l * $signed({ 1'b0, adc_gain })) >>> 1;
wire signed [20:0] adc_rg = (adc_r * $signed({ 1'b0, adc_gain })) >>> 1;
wire signed [20:0] mix_l = sfx_l + opl_l2 + adc_lg;
wire signed [20:0] mix_r = sfx_r + opl_r2 + adc_rg;
always @(posedge clk_a) begin
	snd_l <= (mix_l > 21'sd32767) ? 16'sh7FFF : (mix_l < -21'sd32768) ? 16'sh8000 : mix_l[15:0];
	snd_r <= (mix_r > 21'sd32767) ? 16'sh7FFF : (mix_r < -21'sd32768) ? 16'sh8000 : mix_r[15:0];
end

endmodule
