//Copyright 1986-2020 Xilinx, Inc. All Rights Reserved.
//--------------------------------------------------------------------------------
//Tool Version: Vivado v.2020.2 (win64) Build 3064766 Wed Nov 18 09:12:45 MST 2020
//Date        : Tue Oct 27 19:31:28 2026
//Host        : DESKTOP-SV0I9KV running 64-bit major release  (build 9200)
//Command     : generate_target finn_pdln.bd
//Design      : finn_pdln
//Purpose     : IP block netlist
//--------------------------------------------------------------------------------
`timescale 1 ps / 1 ps

(* CORE_GENERATION_INFO = "finn_pdln,IP_Integrator,{x_ipVendor=xilinx.com,x_ipLibrary=BlockDiagram,x_ipName=finn_pdln,x_ipVersion=1.00.a,x_ipLanguage=VERILOG,numBlks=5,numReposBlks=5,numNonXlnxBlks=0,numHierBlks=0,maxHierDepth=0,numSysgenBlks=0,numHlsBlks=0,numHdlrefBlks=5,numPkgbdBlks=0,bdsource=USER,synth_mode=OOC_per_IP}" *) (* HW_HANDOFF = "finn_pdln.hwdef" *) 
module finn_pdln
   (ap_clk,
    ap_rst_n,
    m_axis_0_tdata,
    m_axis_0_tready,
    m_axis_0_tvalid,
    s_axilite_0_araddr,
    s_axilite_0_arready,
    s_axilite_0_arvalid,
    s_axilite_0_awaddr,
    s_axilite_0_awready,
    s_axilite_0_awvalid,
    s_axilite_0_bready,
    s_axilite_0_bresp,
    s_axilite_0_bvalid,
    s_axilite_0_rdata,
    s_axilite_0_rready,
    s_axilite_0_rresp,
    s_axilite_0_rvalid,
    s_axilite_0_wdata,
    s_axilite_0_wready,
    s_axilite_0_wstrb,
    s_axilite_0_wvalid,
    s_axis_0_tdata,
    s_axis_0_tready,
    s_axis_0_tvalid,
    s_axis_1_tdata,
    s_axis_1_tready,
    s_axis_1_tvalid,
    sim_finish);
  (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 CLK.AP_CLK CLK" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME CLK.AP_CLK, ASSOCIATED_BUSIF s_axilite_0:s_axis_0:s_axis_1:m_axis_0, ASSOCIATED_RESET ap_rst_n, CLK_DOMAIN finn_pdln_ap_clk_0, FREQ_HZ 150000000, FREQ_TOLERANCE_HZ 0, INSERT_VIP 0, PHASE 0.000" *) input ap_clk;
  (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 RST.AP_RST_N RST" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME RST.AP_RST_N, INSERT_VIP 0, POLARITY ACTIVE_LOW" *) input ap_rst_n;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis_0 " *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME m_axis_0, CLK_DOMAIN finn_pdln_ap_clk_0, FREQ_HZ 150000000, HAS_TKEEP 0, HAS_TLAST 0, HAS_TREADY 1, HAS_TSTRB 0, INSERT_VIP 0, LAYERED_METADATA undef, PHASE 0.000, TDATA_NUM_BYTES 2, TDEST_WIDTH 0, TID_WIDTH 0, TUSER_WIDTH 0" *) output [15:0]m_axis_0_tdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis_0 " *) input m_axis_0_tready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 m_axis_0 " *) output m_axis_0_tvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 ARADDR" *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axilite_0, ADDR_WIDTH 16, ARUSER_WIDTH 0, AWUSER_WIDTH 0, BUSER_WIDTH 0, CLK_DOMAIN finn_pdln_ap_clk_0, DATA_WIDTH 32, FREQ_HZ 150000000, HAS_BRESP 1, HAS_BURST 0, HAS_CACHE 0, HAS_LOCK 0, HAS_PROT 0, HAS_QOS 0, HAS_REGION 0, HAS_RRESP 1, HAS_WSTRB 1, ID_WIDTH 0, INSERT_VIP 0, MAX_BURST_LENGTH 1, NUM_READ_OUTSTANDING 1, NUM_READ_THREADS 1, NUM_WRITE_OUTSTANDING 1, NUM_WRITE_THREADS 1, PHASE 0.000, PROTOCOL AXI4LITE, READ_WRITE_MODE READ_WRITE, RUSER_BITS_PER_BYTE 0, RUSER_WIDTH 0, SUPPORTS_NARROW_BURST 0, WUSER_BITS_PER_BYTE 0, WUSER_WIDTH 0" *) input [15:0]s_axilite_0_araddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 ARREADY" *) output s_axilite_0_arready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 ARVALID" *) input s_axilite_0_arvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 AWADDR" *) input [15:0]s_axilite_0_awaddr;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 AWREADY" *) output s_axilite_0_awready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 AWVALID" *) input s_axilite_0_awvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 BREADY" *) input s_axilite_0_bready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 BRESP" *) output [1:0]s_axilite_0_bresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 BVALID" *) output s_axilite_0_bvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RDATA" *) output [31:0]s_axilite_0_rdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RREADY" *) input s_axilite_0_rready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RRESP" *) output [1:0]s_axilite_0_rresp;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 RVALID" *) output s_axilite_0_rvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WDATA" *) input [31:0]s_axilite_0_wdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WREADY" *) output s_axilite_0_wready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WSTRB" *) input [3:0]s_axilite_0_wstrb;
  (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 s_axilite_0 WVALID" *) input s_axilite_0_wvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_0 " *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axis_0, CLK_DOMAIN finn_pdln_ap_clk_0, FREQ_HZ 150000000, HAS_TKEEP 0, HAS_TLAST 0, HAS_TREADY 1, HAS_TSTRB 0, INSERT_VIP 0, LAYERED_METADATA undef, PHASE 0.000, TDATA_NUM_BYTES 2, TDEST_WIDTH 0, TID_WIDTH 0, TUSER_WIDTH 0" *) input [15:0]s_axis_0_tdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_0 " *) output s_axis_0_tready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_0 " *) input s_axis_0_tvalid;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_1 " *) (* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME s_axis_1, CLK_DOMAIN finn_pdln_ap_clk_0, FREQ_HZ 150000000, HAS_TKEEP 0, HAS_TLAST 0, HAS_TREADY 1, HAS_TSTRB 0, INSERT_VIP 0, LAYERED_METADATA undef, PHASE 0.000, TDATA_NUM_BYTES 1, TDEST_WIDTH 0, TID_WIDTH 0, TUSER_WIDTH 0" *) input [7:0]s_axis_1_tdata;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_1 " *) output s_axis_1_tready;
  (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 s_axis_1 " *) input s_axis_1_tvalid;
  input sim_finish;

  wire [7:0]PLFFR0_out0_V_TDATA;
  wire PLFFR0_out0_V_TREADY;
  wire PLFFR0_out0_V_TVALID;
  wire [15:0]PLFFR1_out0_V_TDATA;
  wire PLFFR1_out0_V_TREADY;
  wire PLFFR1_out0_V_TVALID;
  wire [15:0]PLFFR2_out0_V_TDATA;
  wire PLFFR2_out0_V_TREADY;
  wire PLFFR2_out0_V_TVALID;
  wire [15:0]PLLNR0_out0_V_TDATA;
  wire PLLNR0_out0_V_TREADY;
  wire PLLNR0_out0_V_TVALID;
  wire ap_clk_0_1;
  wire ap_rst_n_0_1;
  wire [15:0]in0_V_0_1_TDATA;
  wire in0_V_0_1_TREADY;
  wire in0_V_0_1_TVALID;
  wire [7:0]in0_V_0_2_TDATA;
  wire in0_V_0_2_TREADY;
  wire in0_V_0_2_TVALID;
  wire [15:0]s_axilite_0_1_ARADDR;
  wire s_axilite_0_1_ARREADY;
  wire s_axilite_0_1_ARVALID;
  wire [15:0]s_axilite_0_1_AWADDR;
  wire s_axilite_0_1_AWREADY;
  wire s_axilite_0_1_AWVALID;
  wire s_axilite_0_1_BREADY;
  wire [1:0]s_axilite_0_1_BRESP;
  wire s_axilite_0_1_BVALID;
  wire [31:0]s_axilite_0_1_RDATA;
  wire s_axilite_0_1_RREADY;
  wire [1:0]s_axilite_0_1_RRESP;
  wire s_axilite_0_1_RVALID;
  wire [31:0]s_axilite_0_1_WDATA;
  wire s_axilite_0_1_WREADY;
  wire [3:0]s_axilite_0_1_WSTRB;
  wire s_axilite_0_1_WVALID;
  wire sim_finish_0_1;

  assign PLFFR2_out0_V_TREADY = m_axis_0_tready;
  assign ap_clk_0_1 = ap_clk;
  assign ap_rst_n_0_1 = ap_rst_n;
  assign in0_V_0_1_TDATA = s_axis_0_tdata[15:0];
  assign in0_V_0_1_TVALID = s_axis_0_tvalid;
  assign in0_V_0_2_TDATA = s_axis_1_tdata[7:0];
  assign in0_V_0_2_TVALID = s_axis_1_tvalid;
  assign m_axis_0_tdata[15:0] = PLFFR2_out0_V_TDATA;
  assign m_axis_0_tvalid = PLFFR2_out0_V_TVALID;
  assign s_axilite_0_1_ARADDR = s_axilite_0_araddr[15:0];
  assign s_axilite_0_1_ARVALID = s_axilite_0_arvalid;
  assign s_axilite_0_1_AWADDR = s_axilite_0_awaddr[15:0];
  assign s_axilite_0_1_AWVALID = s_axilite_0_awvalid;
  assign s_axilite_0_1_BREADY = s_axilite_0_bready;
  assign s_axilite_0_1_RREADY = s_axilite_0_rready;
  assign s_axilite_0_1_WDATA = s_axilite_0_wdata[31:0];
  assign s_axilite_0_1_WSTRB = s_axilite_0_wstrb[3:0];
  assign s_axilite_0_1_WVALID = s_axilite_0_wvalid;
  assign s_axilite_0_arready = s_axilite_0_1_ARREADY;
  assign s_axilite_0_awready = s_axilite_0_1_AWREADY;
  assign s_axilite_0_bresp[1:0] = s_axilite_0_1_BRESP;
  assign s_axilite_0_bvalid = s_axilite_0_1_BVALID;
  assign s_axilite_0_rdata[31:0] = s_axilite_0_1_RDATA;
  assign s_axilite_0_rresp[1:0] = s_axilite_0_1_RRESP;
  assign s_axilite_0_rvalid = s_axilite_0_1_RVALID;
  assign s_axilite_0_wready = s_axilite_0_1_WREADY;
  assign s_axis_0_tready = in0_V_0_1_TREADY;
  assign s_axis_1_tready = in0_V_0_2_TREADY;
  assign sim_finish_0_1 = sim_finish;
  finn_pdln_PLFFR0_0 PLFFR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(in0_V_0_2_TDATA),
        .in0_V_TREADY(in0_V_0_2_TREADY),
        .in0_V_TVALID(in0_V_0_2_TVALID),
        .out0_V_TDATA(PLFFR0_out0_V_TDATA),
        .out0_V_TREADY(PLFFR0_out0_V_TREADY),
        .out0_V_TVALID(PLFFR0_out0_V_TVALID));
  finn_pdln_PLFFR1_0 PLFFR1
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(in0_V_0_1_TDATA),
        .in0_V_TREADY(in0_V_0_1_TREADY),
        .in0_V_TVALID(in0_V_0_1_TVALID),
        .out0_V_TDATA(PLFFR1_out0_V_TDATA),
        .out0_V_TREADY(PLFFR1_out0_V_TREADY),
        .out0_V_TVALID(PLFFR1_out0_V_TVALID));
  finn_pdln_PLFFR2_0 PLFFR2
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(PLLNR0_out0_V_TDATA),
        .in0_V_TREADY(PLLNR0_out0_V_TREADY),
        .in0_V_TVALID(PLLNR0_out0_V_TVALID),
        .out0_V_TDATA(PLFFR2_out0_V_TDATA),
        .out0_V_TREADY(PLFFR2_out0_V_TREADY),
        .out0_V_TVALID(PLFFR2_out0_V_TVALID));
  finn_pdln_PLLNR0_0 PLLNR0
       (.ap_clk(ap_clk_0_1),
        .ap_rst_n(ap_rst_n_0_1),
        .in0_V_TDATA(PLFFR1_out0_V_TDATA),
        .in0_V_TREADY(PLFFR1_out0_V_TREADY),
        .in0_V_TVALID(PLFFR1_out0_V_TVALID),
        .in1_V_TDATA(PLFFR0_out0_V_TDATA),
        .in1_V_TREADY(PLFFR0_out0_V_TREADY),
        .in1_V_TVALID(PLFFR0_out0_V_TVALID),
        .out0_V_TDATA(PLLNR0_out0_V_TDATA),
        .out0_V_TREADY(PLLNR0_out0_V_TREADY),
        .out0_V_TVALID(PLLNR0_out0_V_TVALID),
        .s_axilite_ARADDR(s_axilite_0_1_ARADDR),
        .s_axilite_ARREADY(s_axilite_0_1_ARREADY),
        .s_axilite_ARVALID(s_axilite_0_1_ARVALID),
        .s_axilite_AWADDR(s_axilite_0_1_AWADDR),
        .s_axilite_AWREADY(s_axilite_0_1_AWREADY),
        .s_axilite_AWVALID(s_axilite_0_1_AWVALID),
        .s_axilite_BREADY(s_axilite_0_1_BREADY),
        .s_axilite_BRESP(s_axilite_0_1_BRESP),
        .s_axilite_BVALID(s_axilite_0_1_BVALID),
        .s_axilite_RDATA(s_axilite_0_1_RDATA),
        .s_axilite_RREADY(s_axilite_0_1_RREADY),
        .s_axilite_RRESP(s_axilite_0_1_RRESP),
        .s_axilite_RVALID(s_axilite_0_1_RVALID),
        .s_axilite_WDATA(s_axilite_0_1_WDATA),
        .s_axilite_WREADY(s_axilite_0_1_WREADY),
        .s_axilite_WSTRB(s_axilite_0_1_WSTRB),
        .s_axilite_WVALID(s_axilite_0_1_WVALID));
  finn_pdln_sim_ctrl_0_0 sim_ctrl_0
       (.ap_clk(ap_clk_0_1),
        .sim_finish(sim_finish_0_1));
endmodule
