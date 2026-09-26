#include <wolvicmod/wolvicmod.h>

#include "model/dj/directory.h"

namespace zj::dj {

// Directory.scala 组装语义：
//   读：llc/sf 联动（valid 互用对方 rdy 门控，readVec.rdy = 两边 rdy 相与）
//   写：按 Addr.dirBank 分发，write.rdy = (llcWReady|!llc.valid) & (sfWReady|!sf.valid)
//   rRespVec(i) = llcResp.valid & !toRepl（bits 取 llc/sf 各自 wayOH/hit/meta）
//   wResp.llc/sf = resp.valid & toRepl，bank0 优先（同拍 ≤1，见 docs §3.1）

Directory::Directory() {
    for (uint32_t i = 0; i < kDirBanks; ++i) {
        llcs[i].clk = clk;
        sfs[i].clk = clk;
        llcs[i].cfg_bank_id = cfg_bank_id;
        sfs[i].cfg_bank_id = cfg_bank_id;
        llcs[i].dir_bank = static_cast<uint8_t>(i);
        sfs[i].dir_bank = static_cast<uint8_t>(i);
        llcs[i].unlock = unlock;
        sfs[i].unlock = unlock;
    }

    // ---- 读联动 ----
    llcs[0].read.assign().reads(read_0, sfs[0].read_rdy) = [](auto src) {
        auto [read_0, sf_rdy] = src;
        return Valid<DirRdReq>{read_0.valid && sf_rdy, read_0.bits};
    };
    sfs[0].read.assign().reads(read_0, llcs[0].read_rdy) = [](auto src) {
        auto [read_0, llc_rdy] = src;
        return Valid<DirRdReq>{read_0.valid && llc_rdy, read_0.bits};
    };
    read_0_rdy.assign().reads(llcs[0].read_rdy, sfs[0].read_rdy) = [](auto src) {
        auto [llc_rdy, sf_rdy] = src;
        return llc_rdy && sf_rdy;
    };
    llcs[1].read.assign().reads(read_1, sfs[1].read_rdy) = [](auto src) {
        auto [read_1, sf_rdy] = src;
        return Valid<DirRdReq>{read_1.valid && sf_rdy, read_1.bits};
    };
    sfs[1].read.assign().reads(read_1, llcs[1].read_rdy) = [](auto src) {
        auto [read_1, llc_rdy] = src;
        return Valid<DirRdReq>{read_1.valid && llc_rdy, read_1.bits};
    };
    read_1_rdy.assign().reads(llcs[1].read_rdy, sfs[1].read_rdy) = [](auto src) {
        auto [llc_rdy, sf_rdy] = src;
        return llc_rdy && sf_rdy;
    };

    // ---- 写分发 ----
    write_rdy.assign().reads(write, llcs[0].write_rdy, llcs[1].write_rdy, sfs[0].write_rdy,
                             sfs[1].write_rdy) = [](auto src) {
        auto [write, llc0_rdy, llc1_rdy, sf0_rdy, sf1_rdy] = src;
        const bool llcDb = uaDirBank(useAddr(write.bits.llc.addr)) != 0;
        const bool sfDb = uaDirBank(useAddr(write.bits.sf.addr)) != 0;
        const bool llcWReady = llcDb ? llc1_rdy : llc0_rdy;
        const bool sfWReady = sfDb ? sf1_rdy : sf0_rdy;
        return (llcWReady || !write.bits.llcValid) && (sfWReady || !write.bits.sfValid);
    };
    w_write_fire.assign().reads(write, write_rdy) = [](auto src) {
        auto [write, write_rdy] = src;
        return write.valid && write_rdy;
    };
    for (uint32_t i = 0; i < kDirBanks; ++i) {
        llcs[i].write.assign().reads(w_write_fire, write) = [i](auto src) {
            auto [w_write_fire, write] = src;
            const bool db = uaDirBank(useAddr(write.bits.llc.addr)) == i;
            return Valid<DirWrReq>{w_write_fire && write.bits.llcValid && db, write.bits.llc};
        };
        sfs[i].write.assign().reads(w_write_fire, write) = [i](auto src) {
            auto [w_write_fire, write] = src;
            const bool db = uaDirBank(useAddr(write.bits.sf.addr)) == i;
            return Valid<DirWrReq>{w_write_fire && write.bits.sfValid && db, write.bits.sf};
        };
    }

    // ---- 读响应（!toRepl） ----
    rresp_0.assign().reads(llcs[0].resp, sfs[0].resp) = [](auto src) {
        auto [llc_resp, sf_resp] = src;
        DirMsg m;
        m.llc = {llc_resp.bits.wayOH, llc_resp.bits.meta, llc_resp.bits.hit};
        m.sf = {sf_resp.bits.wayOH, sf_resp.bits.meta, sf_resp.bits.hit};
        return Valid<DirMsg>{llc_resp.valid && !llc_resp.bits.toRepl, m};
    };
    rresp_1.assign().reads(llcs[1].resp, sfs[1].resp) = [](auto src) {
        auto [llc_resp, sf_resp] = src;
        DirMsg m;
        m.llc = {llc_resp.bits.wayOH, llc_resp.bits.meta, llc_resp.bits.hit};
        m.sf = {sf_resp.bits.wayOH, sf_resp.bits.meta, sf_resp.bits.hit};
        return Valid<DirMsg>{llc_resp.valid && !llc_resp.bits.toRepl, m};
    };

    // ---- 替换响应（toRepl，bank0 优先） ----
    wresp_llc.assign().reads(llcs[0].resp, llcs[1].resp) = [](auto src) {
        auto [r0, r1] = src;
        const bool c0 = r0.valid && r0.bits.toRepl;
        const bool c1 = r1.valid && r1.bits.toRepl;
        DirResp b = c0 ? r0.bits : r1.bits;
        b.toRepl = false;  // wResp 无 toRepl 字段
        return Valid<DirResp>{c0 || c1, b};
    };
    wresp_sf.assign().reads(sfs[0].resp, sfs[1].resp) = [](auto src) {
        auto [r0, r1] = src;
        const bool c0 = r0.valid && r0.bits.toRepl;
        const bool c1 = r1.valid && r1.bits.toRepl;
        DirResp b = c0 ? r0.bits : r1.bits;
        b.toRepl = false;
        return Valid<DirResp>{c0 || c1, b};
    };
}

}  // namespace zj::dj
