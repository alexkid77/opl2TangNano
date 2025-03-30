// more info at https://www.fpga4fun.com/OPL.html

// specify your FPGA board clock speed here (needs to be at least 8MHz)
`define clkHz 100000000	// for example 25MHz
            
// and specify your FPGA vendor
`define Xilinx
//`define Altera

////////////////////////
module OPL2_demo(
	input clk,
	input RxD,
	output reg LED,
	output PWMaudioR, PWMaudioL,
    output DIN,BCK,LRCK
    
);

// first receive the RS232 data
wire clk100mhz;

wire [7:0] RxD_data;
wire RxD_data_ready, RxD_endofpacket;
 Gowin_rPLL your_instance_name(
        .clkout(clk100mhz), //output clkout
        .clkin(clk) //input clkin
    );

async_receiver #(`clkHz, 115200) RX(
	.clk(clk100mhz),
	.RxD(RxD),
	.RxD_data_ready(RxD_data_ready),
	.RxD_data(RxD_data),
	.RxD_idle(),
	.RxD_endofpacket(RxD_endofpacket)
);

PCM5102 dac(.clk(clk100mhz),
				.left(OPL_snd),
				.right(OPL_snd),
				.din(DIN),
				.bck(BCK),
				.lrck(LRCK) );

// catpure the RS232 data
reg [7:0] OPL_din;  always @(posedge clk100mhz) if(RxD_data_ready) OPL_din <= RxD_data;

// we receive the OPL addresses and data alternatively from the RS232
// so we use a flipflop to keep track
reg OPL_addrT;  always @(posedge clk100mhz) OPL_addrT <= ~RxD_endofpacket & (OPL_addrT ^ RxD_data_ready);

// the OPL works in a different clock domain so we need two additional signals
// first save the OPL cycle address/data type to use in the OPL clock domain
reg OPL_addr;  always @(posedge clk100mhz) if(RxD_data_ready) OPL_addr <= OPL_addrT;

// and toggle a flip-flop to send across the clock domain that data is available
reg OPL_DT;  always @(posedge clk100mhz) OPL_DT <= OPL_DT ^ RxD_data_ready;

////////////////////////
// now generate the 3.579545MHz OPL clock (to match Sound Blaster PC boards)
wire OPL_clk, clkdiv;
clock_divider #(3579545, `clkHz) OPL_clkgen(.clk(clk100mhz), .clkdiv(clkdiv));

`ifdef Xilinx
BUFG OPL_clk_buf(.O(OPL_clk), .I(clkdiv));
`endif

`ifdef Altera
global OPL_clk_buf(.in(clkdiv), .out(OPL_clk));
`endif

////////////////////////
// let's work in the OPL_clk clock domain

// detect if a data is available
reg [2:0] OPL_DTr;  always @(posedge OPL_clk) OPL_DTr <= {OPL_DTr[1:0], OPL_DT};
wire OPL_cs = OPL_DTr[2] ^ OPL_DTr[1];

// and send it to the OPL right away
// (the OPL needs some clock cycles between writes but RS232 at 115200 bauds is slow enough that it's satisfied by design)
wire signed [15:0] OPL_snd;
jtopl #(.OPL_TYPE(2)) myOPL(.rst(1'b0), .clk(OPL_clk), .cen(1'b1), .cs_n(~OPL_cs), .wr_n(1'b0), .din(OPL_din), .addr(OPL_addr), .snd(OPL_snd));
//jt2413  myOPL(.rst(1'b0), .clk(OPL_clk), .cen(1'b1), .cs_n(~OPL_cs), .wr_n(1'b0), .din(OPL_din), .addr(OPL_addr), .snd(OPL_snd));
// generate the PWM pulses from the OPL audio sample output
wire [15:0] OPL_snd_unsigned = (OPL_snd ^ 16'h8000);
reg [16:0] PWM_acc;  always @(posedge OPL_clk) PWM_acc <= PWM_acc[15:0] + OPL_snd_unsigned;
assign PWMaudioR = PWM_acc[16];
assign PWMaudioL = PWM_acc[16];

// and light up the activity LED
wire activity_detected = OPL_cs & ~OPL_addr & OPL_din[7:4]==4'hA;  // detect note frequency changes
reg [13:0] activity_cnt;  always @(posedge clk100mhz/*OPL_clk*/) activity_cnt <= activity_cnt + (activity_detected | |activity_cnt);
always @(posedge OPL_clk) LED <= |activity_cnt;
endmodule


////////////////////////////////////////////////////////////////////////////////
// clock divider
module clock_divider #(parameter N=17, D=114) (input clk, output reg clkdiv);
function integer clog2; input integer value; integer temp; begin temp=value-1; for (clog2=0; temp>0; clog2=clog2+1) begin temp=temp>>1; end end endfunction
localparam W = clog2(D/2-N);
reg [W-1:0] cnt;
wire [W-1:0] cnt_next1 = cnt + N;
wire [W:0] cnt_next2 = cnt + (N-D/2);
wire ovf = ~cnt_next2[W];
always @(posedge clk) cnt <= ovf ? cnt_next2[W-1:0] : cnt_next1;
always @(posedge clk) clkdiv <= clkdiv ^ ovf;
endmodule

module PCM5102(clk,left,right,din,bck,lrck);
	input 			clk;			// sysclk 100MHz
	input [15:0]	left,right;		// left and right 16bit samples Uint16
	output 	reg		din;			// pin on pcm5102 data
	output 	reg		bck;			// pin on pcm5102 bit clock
	output 	reg		lrck;			// pin on pcm5102 l/r clock can be used outside of this module to create new samples
	
	parameter DAC_CLK_DIV_BITS = 2;	// 1 = ca 384Khz, 2 = 192Khz, 3 = 96Khz, 4 = 48Khz 

	reg [DAC_CLK_DIV_BITS:0]	i2s_clk;			// 2 Bit Counter 48MHz -> 6,0 MHz bck = ca 187,5 Khz SampleRate 4% tolerance ok by datasheet
	always @(posedge clk) begin
		i2s_clk 	<= i2s_clk + 1;
	end	

	reg [15:0] l2c;
	reg [15:0] r2c;

	always @(negedge i2sword[5]) begin
		l2c <= left;
		r2c <= right; 
	end	

	reg [5:0]   i2sword = 0;		// 6 bit = 16 steps for left + right
	always @(negedge i2s_clk[DAC_CLK_DIV_BITS]) begin
		lrck	 	<= i2sword[5];
		din 		<= lrck ? r2c[16 - i2sword[4:1]] : l2c[16 - i2sword[4:1]];	// blit data bits
		bck			<= i2sword[0];
		i2sword		<= i2sword + 1;
	end	
endmodule
