// WolvicZjBB：wolvicmod ZhuJiang L3 模型（WolvicZjTop）的 Verilog DPI-C 薄壳。
//
// 端口与 chisel BlackBox（top/WolvicZjBB.scala）逐叶一致；经 verilator libdir
// （make emu RTL_INCLUDE=<本目录>，difftest handle_rtl_include_path → -y）注入。
//
// 时序方案（单调用，前提 = 模型边界零组合穿透，proj tests/test_comb_audit 常驻
// 审计）：每个 posedge 调一次 wolvic_zj_step——C++ 侧以边沿前输入采样驱动模型、
// clk 0→1 提交状态、返回新输出到 t_out；o_out <= t_out 经 NBA 提交，act region
// 内边界输出保持边沿前值，与全寄存边界语义一致。复位期间不调用（模型构造态 =
// 复位完成态，与 P4b golden trace 对齐语义相同）；initial 调 wolvic_zj_peek
// 取初态输出。
//
// 打包约定（in_concat / WolvicZjOut 与 dpi/csrc/wolvic_zj_dpi.cpp 的
// pack/unpack 顺序一一对应，改动必须双侧同步）：
//   - CHI 通道 bits 按模型 pack() 布局（model/flit/xs_flit.h：声明序、先声明
//     占高位）：CHIREQ 118b / CHIRSP 66b / CHIDAT 367b / CHISNP 102b；
//   - 被模型裁剪的 BlackBox 字段（REQ 的 returnNID/stashNIDValid/returnTxnID/
//     ns/likelyshared/allowRetry/pCrdType/lpIDWithPadding/tagOp/traceTag/
//     mpam_{perfMonGroup,mpamNS}、DAT 的 ccID/tagOp/tag/tu/traceTag/rsvdc、
//     SNP 的 ns/traceTag/mpam_*）输入侧丢弃、输出侧绑 0——与 ZhuJiangBridge
//     map* 的读取集一致。

import "DPI-C" function void wolvic_zj_step(
    input  bit [1106:0] in_pack,
    output bit [1442:0] out_pack
);

import "DPI-C" function void wolvic_zj_peek(
    output bit [1442:0] out_pack
);

module WolvicZjBB (
    input              clock,
    input              reset,
    // rn[0] tx（tile → L3）
    output             rn_0_tx_req_ready,
    input              rn_0_tx_req_valid,
    input  [3:0]       rn_0_tx_req_bits_qos,
    input  [10:0]      rn_0_tx_req_bits_tgtID,
    input  [10:0]      rn_0_tx_req_bits_srcID,
    input  [11:0]      rn_0_tx_req_bits_txnID,
    input  [10:0]      rn_0_tx_req_bits_returnNID,
    input              rn_0_tx_req_bits_stashNIDValid,
    input  [11:0]      rn_0_tx_req_bits_returnTxnID,
    input  [6:0]       rn_0_tx_req_bits_opcode,
    input  [2:0]       rn_0_tx_req_bits_size,
    input  [47:0]      rn_0_tx_req_bits_addr,
    input              rn_0_tx_req_bits_ns,
    input              rn_0_tx_req_bits_likelyshared,
    input              rn_0_tx_req_bits_allowRetry,
    input  [1:0]       rn_0_tx_req_bits_order,
    input  [3:0]       rn_0_tx_req_bits_pCrdType,
    input              rn_0_tx_req_bits_memAttr_allocate,
    input              rn_0_tx_req_bits_memAttr_cacheable,
    input              rn_0_tx_req_bits_memAttr_device,
    input              rn_0_tx_req_bits_memAttr_ewa,
    input              rn_0_tx_req_bits_snpAttr,
    input  [7:0]       rn_0_tx_req_bits_lpIDWithPadding,
    input              rn_0_tx_req_bits_snoopMe,
    input              rn_0_tx_req_bits_expCompAck,
    input  [1:0]       rn_0_tx_req_bits_tagOp,
    input              rn_0_tx_req_bits_traceTag,
    input              rn_0_tx_req_bits_mpam_perfMonGroup,
    input  [8:0]       rn_0_tx_req_bits_mpam_partID,
    input              rn_0_tx_req_bits_mpam_mpamNS,
    input  [3:0]       rn_0_tx_req_bits_rsvdc,
    output             rn_0_tx_rsp_ready,
    input              rn_0_tx_rsp_valid,
    input  [3:0]       rn_0_tx_rsp_bits_qos,
    input  [10:0]      rn_0_tx_rsp_bits_tgtID,
    input  [10:0]      rn_0_tx_rsp_bits_srcID,
    input  [11:0]      rn_0_tx_rsp_bits_txnID,
    input  [4:0]       rn_0_tx_rsp_bits_opcode,
    input  [1:0]       rn_0_tx_rsp_bits_respErr,
    input  [2:0]       rn_0_tx_rsp_bits_resp,
    input  [2:0]       rn_0_tx_rsp_bits_fwdState,
    input  [2:0]       rn_0_tx_rsp_bits_cBusy,
    input  [11:0]      rn_0_tx_rsp_bits_dbID,
    input  [3:0]       rn_0_tx_rsp_bits_pCrdType,
    input  [1:0]       rn_0_tx_rsp_bits_tagOp,
    input              rn_0_tx_rsp_bits_traceTag,
    output             rn_0_tx_dat_ready,
    input              rn_0_tx_dat_valid,
    input  [3:0]       rn_0_tx_dat_bits_qos,
    input  [10:0]      rn_0_tx_dat_bits_tgtID,
    input  [10:0]      rn_0_tx_dat_bits_srcID,
    input  [11:0]      rn_0_tx_dat_bits_txnID,
    input  [10:0]      rn_0_tx_dat_bits_homeNID,
    input  [3:0]       rn_0_tx_dat_bits_opcode,
    input  [1:0]       rn_0_tx_dat_bits_respErr,
    input  [2:0]       rn_0_tx_dat_bits_resp,
    input  [3:0]       rn_0_tx_dat_bits_dataSource,
    input  [2:0]       rn_0_tx_dat_bits_cBusy,
    input  [11:0]      rn_0_tx_dat_bits_dbID,
    input  [1:0]       rn_0_tx_dat_bits_ccID,
    input  [1:0]       rn_0_tx_dat_bits_dataID,
    input  [1:0]       rn_0_tx_dat_bits_tagOp,
    input  [7:0]       rn_0_tx_dat_bits_tag,
    input  [1:0]       rn_0_tx_dat_bits_tu,
    input              rn_0_tx_dat_bits_traceTag,
    input  [3:0]       rn_0_tx_dat_bits_rsvdc,
    input  [31:0]      rn_0_tx_dat_bits_be,
    input  [255:0]     rn_0_tx_dat_bits_data,
    // rn[0] rx（L3 → tile）
    input              rn_0_rx_rsp_ready,
    output             rn_0_rx_rsp_valid,
    output [3:0]       rn_0_rx_rsp_bits_qos,
    output [10:0]      rn_0_rx_rsp_bits_tgtID,
    output [10:0]      rn_0_rx_rsp_bits_srcID,
    output [11:0]      rn_0_rx_rsp_bits_txnID,
    output [4:0]       rn_0_rx_rsp_bits_opcode,
    output [1:0]       rn_0_rx_rsp_bits_respErr,
    output [2:0]       rn_0_rx_rsp_bits_resp,
    output [2:0]       rn_0_rx_rsp_bits_fwdState,
    output [2:0]       rn_0_rx_rsp_bits_cBusy,
    output [11:0]      rn_0_rx_rsp_bits_dbID,
    output [3:0]       rn_0_rx_rsp_bits_pCrdType,
    output [1:0]       rn_0_rx_rsp_bits_tagOp,
    output             rn_0_rx_rsp_bits_traceTag,
    input              rn_0_rx_dat_ready,
    output             rn_0_rx_dat_valid,
    output [3:0]       rn_0_rx_dat_bits_qos,
    output [10:0]      rn_0_rx_dat_bits_tgtID,
    output [10:0]      rn_0_rx_dat_bits_srcID,
    output [11:0]      rn_0_rx_dat_bits_txnID,
    output [10:0]      rn_0_rx_dat_bits_homeNID,
    output [3:0]       rn_0_rx_dat_bits_opcode,
    output [1:0]       rn_0_rx_dat_bits_respErr,
    output [2:0]       rn_0_rx_dat_bits_resp,
    output [3:0]       rn_0_rx_dat_bits_dataSource,
    output [2:0]       rn_0_rx_dat_bits_cBusy,
    output [11:0]      rn_0_rx_dat_bits_dbID,
    output [1:0]       rn_0_rx_dat_bits_ccID,
    output [1:0]       rn_0_rx_dat_bits_dataID,
    output [1:0]       rn_0_rx_dat_bits_tagOp,
    output [7:0]       rn_0_rx_dat_bits_tag,
    output [1:0]       rn_0_rx_dat_bits_tu,
    output             rn_0_rx_dat_bits_traceTag,
    output [3:0]       rn_0_rx_dat_bits_rsvdc,
    output [31:0]      rn_0_rx_dat_bits_be,
    output [255:0]     rn_0_rx_dat_bits_data,
    input              rn_0_rx_snp_ready,
    output             rn_0_rx_snp_valid,
    output [3:0]       rn_0_rx_snp_bits_qos,
    output [10:0]      rn_0_rx_snp_bits_srcID,
    output [11:0]      rn_0_rx_snp_bits_txnID,
    output [10:0]      rn_0_rx_snp_bits_fwdNID,
    output [11:0]      rn_0_rx_snp_bits_fwdTxnID,
    output [4:0]       rn_0_rx_snp_bits_opcode,
    output [44:0]      rn_0_rx_snp_bits_addr,
    output             rn_0_rx_snp_bits_ns,
    output             rn_0_rx_snp_bits_doNotGoToSD,
    output             rn_0_rx_snp_bits_retToSrc,
    output             rn_0_rx_snp_bits_traceTag,
    output             rn_0_rx_snp_bits_mpam_perfMonGroup,
    output [8:0]       rn_0_rx_snp_bits_mpam_partID,
    output             rn_0_rx_snp_bits_mpam_mpamNS,
    // ddrc（memAXI）
    input              ddrc_awready,
    output             ddrc_awvalid,
    output [5:0]       ddrc_awid,
    output [48:0]      ddrc_awaddr,
    output [7:0]       ddrc_awlen,
    output [2:0]       ddrc_awsize,
    output [1:0]       ddrc_awburst,
    output             ddrc_awlock,
    output [3:0]       ddrc_awcache,
    output [2:0]       ddrc_awprot,
    output [3:0]       ddrc_awqos,
    input              ddrc_wready,
    output             ddrc_wvalid,
    output [255:0]     ddrc_wdata,
    output [31:0]      ddrc_wstrb,
    output             ddrc_wlast,
    output             ddrc_bready,
    input              ddrc_bvalid,
    input  [5:0]       ddrc_bid,
    input  [1:0]       ddrc_bresp,
    input              ddrc_arready,
    output             ddrc_arvalid,
    output [5:0]       ddrc_arid,
    output [48:0]      ddrc_araddr,
    output [7:0]       ddrc_arlen,
    output [2:0]       ddrc_arsize,
    output [1:0]       ddrc_arburst,
    output             ddrc_arlock,
    output [3:0]       ddrc_arcache,
    output [2:0]       ddrc_arprot,
    output [3:0]       ddrc_arqos,
    output             ddrc_rready,
    input              ddrc_rvalid,
    input  [5:0]       ddrc_rid,
    input  [255:0]     ddrc_rdata,
    input  [1:0]       ddrc_rresp,
    input              ddrc_rlast,
    // peri[0]（cfgAXI）
    input              peri_0_awready,
    output             peri_0_awvalid,
    output [2:0]       peri_0_awid,
    output [48:0]      peri_0_awaddr,
    output [7:0]       peri_0_awlen,
    output [2:0]       peri_0_awsize,
    output [1:0]       peri_0_awburst,
    output             peri_0_awlock,
    output [3:0]       peri_0_awcache,
    output [2:0]       peri_0_awprot,
    output [3:0]       peri_0_awqos,
    input              peri_0_wready,
    output             peri_0_wvalid,
    output [255:0]     peri_0_wdata,
    output [31:0]      peri_0_wstrb,
    output             peri_0_wlast,
    output             peri_0_bready,
    input              peri_0_bvalid,
    input  [2:0]       peri_0_bid,
    input  [1:0]       peri_0_bresp,
    input              peri_0_arready,
    output             peri_0_arvalid,
    output [2:0]       peri_0_arid,
    output [48:0]      peri_0_araddr,
    output [7:0]       peri_0_arlen,
    output [2:0]       peri_0_arsize,
    output [1:0]       peri_0_arburst,
    output             peri_0_arlock,
    output [3:0]       peri_0_arcache,
    output [2:0]       peri_0_arprot,
    output [3:0]       peri_0_arqos,
    output             peri_0_rready,
    input              peri_0_rvalid,
    input  [2:0]       peri_0_rid,
    input  [255:0]     peri_0_rdata,
    input  [1:0]       peri_0_rresp,
    input              peri_0_rlast
);

    // 模型输出包（声明序 = pack 序：先声明占高位）。与 C++ packOutputs 对应。
    typedef struct packed {
        // L2 CHI 输出
        logic         tx_req_ready;
        logic         tx_rsp_ready;
        logic         tx_dat_ready;
        logic         rx_rsp_valid;
        logic [65:0]  rx_rsp_bits;
        logic         rx_dat_valid;
        logic [366:0] rx_dat_bits;
        logic         rx_snp_valid;
        logic [101:0] rx_snp_bits;
        // memAXI 输出
        logic         mem_awvalid;
        logic [5:0]   mem_awid;
        logic [48:0]  mem_awaddr;
        logic [7:0]   mem_awlen;
        logic [2:0]   mem_awsize;
        logic [1:0]   mem_awburst;
        logic         mem_awlock;
        logic [3:0]   mem_awcache;
        logic [2:0]   mem_awprot;
        logic [3:0]   mem_awqos;
        logic         mem_wvalid;
        logic [255:0] mem_wdata;
        logic [31:0]  mem_wstrb;
        logic         mem_wlast;
        logic         mem_bready;
        logic         mem_arvalid;
        logic [5:0]   mem_arid;
        logic [48:0]  mem_araddr;
        logic [7:0]   mem_arlen;
        logic [2:0]   mem_arsize;
        logic [1:0]   mem_arburst;
        logic         mem_arlock;
        logic [3:0]   mem_arcache;
        logic [2:0]   mem_arprot;
        logic [3:0]   mem_arqos;
        logic         mem_rready;
        // cfgAXI 输出
        logic         cfg_awvalid;
        logic [2:0]   cfg_awid;
        logic [48:0]  cfg_awaddr;
        logic [7:0]   cfg_awlen;
        logic [2:0]   cfg_awsize;
        logic [1:0]   cfg_awburst;
        logic         cfg_awlock;
        logic [3:0]   cfg_awcache;
        logic [2:0]   cfg_awprot;
        logic [3:0]   cfg_awqos;
        logic         cfg_wvalid;
        logic [255:0] cfg_wdata;
        logic [31:0]  cfg_wstrb;
        logic         cfg_wlast;
        logic         cfg_bready;
        logic         cfg_arvalid;
        logic [2:0]   cfg_arid;
        logic [48:0]  cfg_araddr;
        logic [7:0]   cfg_arlen;
        logic [2:0]   cfg_arsize;
        logic [1:0]   cfg_arburst;
        logic         cfg_arlock;
        logic [3:0]   cfg_arcache;
        logic [2:0]   cfg_arprot;
        logic [3:0]   cfg_arqos;
        logic         cfg_rready;
    } WolvicZjOut;

    initial begin
        if ($bits(WolvicZjOut) != 1443) $fatal(1, "WolvicZjOut width drift");
    end

    // ---- CHI bits 打包（模型 pack() 布局：先声明占高位） ----
    wire [117:0] tx_req_bits = {
        rn_0_tx_req_bits_qos, rn_0_tx_req_bits_tgtID, rn_0_tx_req_bits_srcID,
        rn_0_tx_req_bits_txnID, rn_0_tx_req_bits_opcode, rn_0_tx_req_bits_size,
        rn_0_tx_req_bits_addr, rn_0_tx_req_bits_order,
        rn_0_tx_req_bits_memAttr_allocate, rn_0_tx_req_bits_memAttr_cacheable,
        rn_0_tx_req_bits_memAttr_device, rn_0_tx_req_bits_memAttr_ewa,
        rn_0_tx_req_bits_snpAttr, rn_0_tx_req_bits_snoopMe,
        rn_0_tx_req_bits_expCompAck, rn_0_tx_req_bits_mpam_partID,
        rn_0_tx_req_bits_rsvdc
    };
    wire [65:0] tx_rsp_bits = {
        rn_0_tx_rsp_bits_qos, rn_0_tx_rsp_bits_tgtID, rn_0_tx_rsp_bits_srcID,
        rn_0_tx_rsp_bits_txnID, rn_0_tx_rsp_bits_opcode, rn_0_tx_rsp_bits_respErr,
        rn_0_tx_rsp_bits_resp, rn_0_tx_rsp_bits_fwdState, rn_0_tx_rsp_bits_cBusy,
        rn_0_tx_rsp_bits_dbID
    };
    wire [366:0] tx_dat_bits = {
        rn_0_tx_dat_bits_qos, rn_0_tx_dat_bits_tgtID, rn_0_tx_dat_bits_srcID,
        rn_0_tx_dat_bits_txnID, rn_0_tx_dat_bits_homeNID, rn_0_tx_dat_bits_opcode,
        rn_0_tx_dat_bits_respErr, rn_0_tx_dat_bits_resp, rn_0_tx_dat_bits_dataSource,
        rn_0_tx_dat_bits_cBusy, rn_0_tx_dat_bits_dbID, rn_0_tx_dat_bits_dataID,
        rn_0_tx_dat_bits_be, rn_0_tx_dat_bits_data
    };

    // ---- 输入打包（顺序与 C++ unpackInputs 对应：先占高位） ----
    wire [1106:0] in_pack = {
        rn_0_tx_req_valid, tx_req_bits,
        rn_0_tx_rsp_valid, tx_rsp_bits,
        rn_0_tx_dat_valid, tx_dat_bits,
        rn_0_rx_rsp_ready, rn_0_rx_dat_ready, rn_0_rx_snp_ready,
        ddrc_awready, ddrc_wready,
        ddrc_bvalid, ddrc_bid, ddrc_bresp,
        ddrc_arready,
        ddrc_rvalid, ddrc_rid, ddrc_rdata, ddrc_rresp, ddrc_rlast,
        peri_0_awready, peri_0_wready,
        peri_0_bvalid, peri_0_bid, peri_0_bresp,
        peri_0_arready,
        peri_0_rvalid, peri_0_rid, peri_0_rdata, peri_0_rresp, peri_0_rlast
    };

    WolvicZjOut t_out;  // DPI 返回暂存（act region 内被写）
    WolvicZjOut o_out;  // NBA 提交后的边界输出

    initial begin
        wolvic_zj_peek(t_out);
        o_out = t_out;
    end

    always @(posedge clock) begin
        if (!reset) begin
            wolvic_zj_step(in_pack, t_out);
            o_out <= t_out;
        end
    end

    // ---- 输出驱动 ----
    assign rn_0_tx_req_ready = o_out.tx_req_ready;
    assign rn_0_tx_rsp_ready = o_out.tx_rsp_ready;
    assign rn_0_tx_dat_ready = o_out.tx_dat_ready;

    assign rn_0_rx_rsp_valid         = o_out.rx_rsp_valid;
    assign {rn_0_rx_rsp_bits_qos, rn_0_rx_rsp_bits_tgtID, rn_0_rx_rsp_bits_srcID,
            rn_0_rx_rsp_bits_txnID, rn_0_rx_rsp_bits_opcode, rn_0_rx_rsp_bits_respErr,
            rn_0_rx_rsp_bits_resp, rn_0_rx_rsp_bits_fwdState, rn_0_rx_rsp_bits_cBusy,
            rn_0_rx_rsp_bits_dbID}   = o_out.rx_rsp_bits;
    assign rn_0_rx_rsp_bits_pCrdType = 4'b0;
    assign rn_0_rx_rsp_bits_tagOp    = 2'b0;
    assign rn_0_rx_rsp_bits_traceTag = 1'b0;

    assign rn_0_rx_dat_valid         = o_out.rx_dat_valid;
    assign {rn_0_rx_dat_bits_qos, rn_0_rx_dat_bits_tgtID, rn_0_rx_dat_bits_srcID,
            rn_0_rx_dat_bits_txnID, rn_0_rx_dat_bits_homeNID, rn_0_rx_dat_bits_opcode,
            rn_0_rx_dat_bits_respErr, rn_0_rx_dat_bits_resp, rn_0_rx_dat_bits_dataSource,
            rn_0_rx_dat_bits_cBusy, rn_0_rx_dat_bits_dbID, rn_0_rx_dat_bits_dataID,
            rn_0_rx_dat_bits_be, rn_0_rx_dat_bits_data} = o_out.rx_dat_bits;
    assign rn_0_rx_dat_bits_ccID     = 2'b0;
    assign rn_0_rx_dat_bits_tagOp    = 2'b0;
    assign rn_0_rx_dat_bits_tag      = 8'b0;
    assign rn_0_rx_dat_bits_tu       = 2'b0;
    assign rn_0_rx_dat_bits_traceTag = 1'b0;
    assign rn_0_rx_dat_bits_rsvdc    = 4'b0;

    assign rn_0_rx_snp_valid         = o_out.rx_snp_valid;
    assign {rn_0_rx_snp_bits_qos, rn_0_rx_snp_bits_srcID, rn_0_rx_snp_bits_txnID,
            rn_0_rx_snp_bits_fwdNID, rn_0_rx_snp_bits_fwdTxnID, rn_0_rx_snp_bits_opcode,
            rn_0_rx_snp_bits_addr, rn_0_rx_snp_bits_doNotGoToSD,
            rn_0_rx_snp_bits_retToSrc} = o_out.rx_snp_bits;
    assign rn_0_rx_snp_bits_ns                = 1'b0;
    assign rn_0_rx_snp_bits_traceTag          = 1'b0;
    assign rn_0_rx_snp_bits_mpam_perfMonGroup = 1'b0;
    assign rn_0_rx_snp_bits_mpam_partID       = 9'b0;
    assign rn_0_rx_snp_bits_mpam_mpamNS       = 1'b0;

    assign ddrc_awvalid = o_out.mem_awvalid;
    assign ddrc_awid    = o_out.mem_awid;
    assign ddrc_awaddr  = o_out.mem_awaddr;
    assign ddrc_awlen   = o_out.mem_awlen;
    assign ddrc_awsize  = o_out.mem_awsize;
    assign ddrc_awburst = o_out.mem_awburst;
    assign ddrc_awlock  = o_out.mem_awlock;
    assign ddrc_awcache = o_out.mem_awcache;
    assign ddrc_awprot  = o_out.mem_awprot;
    assign ddrc_awqos   = o_out.mem_awqos;
    assign ddrc_wvalid  = o_out.mem_wvalid;
    assign ddrc_wdata   = o_out.mem_wdata;
    assign ddrc_wstrb   = o_out.mem_wstrb;
    assign ddrc_wlast   = o_out.mem_wlast;
    assign ddrc_bready  = o_out.mem_bready;
    assign ddrc_arvalid = o_out.mem_arvalid;
    assign ddrc_arid    = o_out.mem_arid;
    assign ddrc_araddr  = o_out.mem_araddr;
    assign ddrc_arlen   = o_out.mem_arlen;
    assign ddrc_arsize  = o_out.mem_arsize;
    assign ddrc_arburst = o_out.mem_arburst;
    assign ddrc_arlock  = o_out.mem_arlock;
    assign ddrc_arcache = o_out.mem_arcache;
    assign ddrc_arprot  = o_out.mem_arprot;
    assign ddrc_arqos   = o_out.mem_arqos;
    assign ddrc_rready  = o_out.mem_rready;

    assign peri_0_awvalid = o_out.cfg_awvalid;
    assign peri_0_awid    = o_out.cfg_awid;
    assign peri_0_awaddr  = o_out.cfg_awaddr;
    assign peri_0_awlen   = o_out.cfg_awlen;
    assign peri_0_awsize  = o_out.cfg_awsize;
    assign peri_0_awburst = o_out.cfg_awburst;
    assign peri_0_awlock  = o_out.cfg_awlock;
    assign peri_0_awcache = o_out.cfg_awcache;
    assign peri_0_awprot  = o_out.cfg_awprot;
    assign peri_0_awqos   = o_out.cfg_awqos;
    assign peri_0_wvalid  = o_out.cfg_wvalid;
    assign peri_0_wdata   = o_out.cfg_wdata;
    assign peri_0_wstrb   = o_out.cfg_wstrb;
    assign peri_0_wlast   = o_out.cfg_wlast;
    assign peri_0_bready  = o_out.cfg_bready;
    assign peri_0_arvalid = o_out.cfg_arvalid;
    assign peri_0_arid    = o_out.cfg_arid;
    assign peri_0_araddr  = o_out.cfg_araddr;
    assign peri_0_arlen   = o_out.cfg_arlen;
    assign peri_0_arsize  = o_out.cfg_arsize;
    assign peri_0_arburst = o_out.cfg_arburst;
    assign peri_0_arlock  = o_out.cfg_arlock;
    assign peri_0_arcache = o_out.cfg_arcache;
    assign peri_0_arprot  = o_out.cfg_arprot;
    assign peri_0_arqos   = o_out.cfg_arqos;
    assign peri_0_rready  = o_out.cfg_rready;

endmodule
