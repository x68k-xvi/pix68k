// 実行状態の保存と復元 (形式は state.h)。各クラスの state() もここにまとめて書く
// (並べる順を変えたら STATE_VERSION を上げる)。
//
// ポインタは保存しない。指している先が自分の中 (コマンドやセンスのバッファ) ならその位置、ディスクのイメージなら
// イメージの中の位置にして保存し、読むときにポインタに戻す。速く読み書きするための表 (CPU の fast page、68030 / 68060 の
// TLB) は保存せず、読んだあとに作り直す。ただし、68060 の MMU が有効な間の TLB と ATC は、実機の ATC と同じく
// 結果を変えるので保存する (Cpu020::stateTlb())。
//
// 音の合成を別のコア (スレッド) で行うとき (x68k/sound_replay.h) の注意:
// Machine の Sound は出力の周波数が 0 になり、OPM はタイマ (割り込みと BUSY) だけを進め、LFO、ノイズ、エンベロープ、
// 位相は進めない (Opm::clockSample(nullptr))。ADPCM は両方で進む。そのまま保存すると、鳴っている音の状態
// (エンベロープや位相) は入らない。合成用の Sound は同じ書き込みとクロックを受けるので、タイマも含めて
// Machine の Sound と同じ状態に加えて音の状態を持つ。そこで、記録を処理し終えてから
//   保存の前に copySoundState(合成用, machine.sound())、読んだあとに copySoundState(machine.sound(), 合成用)
// とする。
#include "x68k/state.h"

#include <string.h>

#ifndef PIX68K_NO_CPU020
#include "m68k/cpu020.h"
#include "m68k/fpu.h"
#endif
#include "x68k/machine.h"

namespace x68k {

namespace {

const char kMagic[8] = {'P', 'I', 'X', '6', '8', 'K', 'S', 'T'};
const size_t kPage = 4096;

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// ヘッダの機種の部分 (+16 から kFingerprintWords 語)
const int kFingerprintWords = 8;
void fingerprint(const Machine& m, uint32_t out[kFingerprintWords]) {
    const Config& c = m.config();
    out[0] = (uint32_t)c.cpuModel;
    out[1] = c.ramSize;
    out[2] = c.highMemSize;
    out[3] = c.localMemSize;
    out[4] = c.cpuHz;
    out[5] = m.iplRom() ? crc32(m.iplRom(), Machine::IPL_SIZE) : 0;
    out[6] = m.scsiRom() ? crc32(m.scsiRom(), Machine::SCSI_ROM_SIZE) : 0;
    out[7] = m.scsiExRom() ? crc32(m.scsiExRom(), Machine::SCSI_EX_ROM_SIZE) : 0;
}

// ステートから読んだ bool のバイトが 0 か 1 か (ほかの値を bool として読むと未定義の動作になるので、バイトで見る)
bool boolOk(const bool& b) {
    uint8_t v;
    memcpy(&v, &b, 1);
    return v <= 1;
}
template <size_t N> bool boolOk(const bool (&b)[N]) {
    for (size_t i = 0; i < N; i++)
        if (!boolOk(b[i])) return false;
    return true;
}

// ポインタも仮想関数も持たないクラスを丸ごと読み書きする (StateIO::raw() と同じ並び)。読むときは、写しに読んで
// ok(写し) が true のときだけ戻す。false なら読まない (CHECK で確かめるので、Machine を変える前に断る)。
// 添字やシフトの数に使う値、bool のバイトを ok で確かめる
template <class T, class Ok> void rawChecked(StateIO& s, T& v, Ok ok) {
    static_assert(__is_trivially_copyable(T), "rawChecked() needs a trivially copyable type");
    static_assert(__has_unique_object_representations(T), "rawChecked() needs a type without padding");
    if (s.saving()) {
        s.bytes(&v, sizeof v);
        return;
    }
    T t(v);
    s.local(&t, sizeof t);
    if (!s.ok()) return;
    if (!ok(t)) s.fail();
    else if (s.applying()) memcpy(&v, &t, sizeof v);
}

} // namespace

// ---------------------------------------------------------------- StateIO

void StateIO::bytes(void* p, size_t n) {
    if (!ok_ || mode_ == SHIFT) return;
    if (mode_ != SIZE && pos_ + n > cap_) {
        ok_ = false;
        return;
    }
    if (mode_ == SAVE) memcpy(buf_ + pos_, p, n);
    else if (mode_ == LOAD) memcpy(p, buf_ + pos_, n);
    pos_ += n;
}

// se (2 バイト)、詰め物 (6 バイト)、m (8 バイト)。X を丸ごと書いていたときと同じ並び
#if !defined(PIX68K_NO_CPU020) || !defined(PIX68K_NO_FPU_BOARD)
void StateIO::io(m68k::fp::X& v) {
    static_assert(sizeof v == 16 && offsetof(m68k::fp::X, m) == 8 && sizeof v.se == 2, "layout of fp::X in the state");
    uint8_t pad[6] = {};
    bytes(&v.se, sizeof v.se);
    bytes(pad, sizeof pad);
    bytes(&v.m, sizeof v.m);
}

#endif

void StateIO::local(void* p, size_t n) {
    if (!ok_ || mode_ == SHIFT) return;
    if (mode_ != SIZE && pos_ + n > cap_) {
        ok_ = false;
        return;
    }
    if (mode_ == SAVE) memcpy(buf_ + pos_, p, n);
    else if (mode_ == LOAD || mode_ == CHECK) memcpy(p, buf_ + pos_, n);
    pos_ += n;
}

void StateIO::tag(uint32_t t) {
    uint32_t v = t;
    local(&v, sizeof v);
    if (v != t) ok_ = false;
}

void StateIO::memory(uint8_t* p, size_t n) {
    if (mode_ == SIZE && fullMemory_) {
        pos_ += n + (n + kPage - 1) / kPage;
        return;
    }
    for (size_t a = 0; a < n && ok_; a += kPage) {
        const size_t len = n - a < kPage ? n - a : kPage;
        uint8_t used = 0;
        if (saving()) {
            for (size_t i = 0; i < len; i++)
                if (p[a + i]) {
                    used = 1;
                    break;
                }
        }
        local(&used, 1);
        if (used > 1) ok_ = false;
        if (used) bytes(p + a, len);
        else if (applying()) memset(p + a, 0, len);
    }
}

// ---------------------------------------------------------------- CPU

} // namespace x68k

void m68k::Cpu::state(x68k::StateIO& s) {
    s.tag(0x30303036); // "6000"
    s.io(d_);
    s.io(a_);
    s.io(inactiveSp_);
    s.io(pc_);
    s.io(ird_);
    s.io(irc_);
    // フラグは 0 か 1、割り込みのレベルは 0 から 7 (SR を組み立てるときにそのままシフトして使う)
    s.range(flagX_, 0u, 1u);
    s.range(flagN_, 0u, 1u);
    s.range(flagZ_, 0u, 1u);
    s.range(flagV_, 0u, 1u);
    s.range(flagC_, 0u, 1u);
    s.io(sflag_);
    s.io(tflag_);
    s.range(imask_, 0, 7);
    // 時刻は、上限 (state.h の STATE_CPU_CYCLES_MAX) を超えていれば、小さい値に直す (64 ビットの上限の近くでは、
    // Machine::run() の進める先の時刻の足し算があふれ、進まずに戻る)。戻した量は、時刻で持っているほかの値
    // (StateIO::time()) にも使う。68030 / 68060 のときは、あとで読む Cpu020::state() が決め直す
    s.io(cycles_);
    if (s.applying()) {
        const uint64_t kept = x68k::stateKeptTime(cycles_, x68k::STATE_CPU_CYCLES_MAX, x68k::STATE_CPU_TIME_PAST);
        s.setTimeShift(cycles_ - kept);
        cycles_ = kept;
    }
    s.range(ipl_, 0, 7);
    s.range(lastIpl_, 0, 7);
    s.io(nmiPending_);
    s.io(halted_);
    s.io(stopped_);
    s.io(inGroup0_);
    s.io(faultPc_);
    s.io(faultNotInstr_);
    s.io(noTrace_);
    s.io(inIack_);
    s.io(curOp_);
    s.io(traceAtStart_);
}

#ifndef PIX68K_NO_CPU020
void m68k::Cpu020::state(x68k::StateIO& s) {
    s.tag(0x30333036); // "6030"
    s.io(d_);
    s.io(a_);
    s.io(usp_);
    s.io(isp_);
    s.io(msp_);
    // PC は偶数 (奇数への分岐は jumpTo() がアドレスエラーにするので、命令の間で奇数になることはない)。命令の語は
    // 偶数の前提で 2 バイトずつ読むので、奇数だとページの終わりで 1 バイト外を読む。奇数なら読まない
    uint32_t pc = pc_;
    s.loc(pc);
    if (!s.saving()) {
        if (pc & 1) s.fail();
        else if (s.applying()) pc_ = pc;
    }
    s.io(pc0_);
    s.io(op_);
    s.range(flagX_, 0u, 1u);
    s.range(flagN_, 0u, 1u);
    s.range(flagZ_, 0u, 1u);
    s.range(flagV_, 0u, 1u);
    s.range(flagC_, 0u, 1u);
    s.io(sflag_);
    s.io(mflag_);
    s.range(trace_, 0, 3);
    s.range(imask_, 0, 7);
    s.io(vbr_);
    s.range(sfc_, 0u, 7u); // ファンクションコード (MOVEC は下位 3 ビットだけを入れる)
    s.range(dfc_, 0u, 7u);
    s.io(cacr_);
    s.io(caar_);
    s.io(tc030_);
    s.io(tt030_);
    s.io(mmusr030_);
    s.io(crp030_);
    s.io(srp030_);
    s.io(pcr_);
    s.io(buscr_);
    s.io(tc060_);
    s.io(itt_);
    s.io(dtt_);
    s.io(urp_);
    s.io(srp060_);
    // 時刻 (1/SUB サイクルの単位) は、上限 (state.h の STATE_CPU020_CYCLES_MAX) を超えていれば、小さい値に直す。
    // 64 ビットの上限の近くでは、run() の進める先の時刻の足し算があふれ、CPU が進まないまま Machine::run() が
    // 戻らなくなる。戻すのはサイクルの単位 (サイクル数とその端数に分けて、サイクル数のほうを直す) なので、サイクルの
    // 端数は変わらない。戻したサイクル数は、時刻で持っているほかの値 (StateIO::time()) にも使う。
    // 版 14 までは 1/800 サイクルの単位 (STATE_CPU020_SUB_OLD) で書いてある。読むときは今の単位に直し (端数は 16 倍)、
    // 古い版の並びで書くときは 1/800 サイクルに直す (16 で割り切れない端数は捨てる。既定の速さでは端数はいつも 16 の倍数)
    const uint64_t unit = s.version() >= x68k::STATE_CPU020_SUB ? (uint64_t)SUB : x68k::STATE_CPU020_SUB_OLD;
    uint64_t t = s.saving() ? sub_ / (SUB / unit) : 0;
    s.io(t);
    if (s.applying()) {
        const uint64_t c = t / unit, frac = t % unit * (SUB / unit);
        const uint64_t max = x68k::STATE_CPU020_CYCLES_MAX;
        // 上限ちょうどのサイクル数で端数があるものも、上限を超えている (1/SUB サイクルの単位で stateKeptTime() に
        // 渡したのと同じになる)
        const bool over = c > max || (c == max && frac);
        const uint64_t kept = over ? x68k::STATE_CPU_TIME_PAST + c % (max / 2) : c;
        s.setTimeShift(c - kept);
        sub_ = kept * SUB + frac;
    }
    s.range(ipl_, 0, 7);
    s.range(lastIpl_, 0, 7);
    s.io(nmiPending_);
    s.io(halted_);
    s.io(stopped_);
    s.io(inException_);
    s.io(inIack_);
    // busCycle_ は、Bus に出す読み書きのたびに先に入れ直す値で、命令と命令の間 (保存するとき) では使わない。TLB に
    // ないページを読んだかどうかで残る値が変わる (ステートを読んだあとは TLB が空なので変わる) ので、0 にして保存する
    uint8_t busCycle = s.saving() ? (uint8_t)BUS_LONG : busCycle_;
    s.loc(busCycle);
    if (s.applying()) busCycle_ = busCycle;
    s.io(flow_);
    s.io(noTrace_);
    s.io(inTableWalk_);
    s.io(walkAddr_);
    s.io(walkWrite_);
    s.io(walkFetch_);
    s.io(walkFc_);
    s.io(logical_);
    s.io(logicalWrite_);
    // 命令の始めの写し (undoA_、undoCcr_、undoS_、undoM_) は、命令の外では使わない (次の命令が、要るなら取り直す)。
    // どの命令が最後に写したかで中身が変わらないよう、保存するときは今の値にそろえる
    if (!s.applying()) {
        for (int i = 0; i < 8; i++) undoA_[i] = a_[i];
        undoCcr_[0] = flagX_, undoCcr_[1] = flagN_, undoCcr_[2] = flagZ_, undoCcr_[3] = flagV_, undoCcr_[4] = flagC_;
        undoS_ = sflag_, undoM_ = mflag_;
    }
    s.io(undoA_);
    for (uint32_t& f : undoCcr_) s.range(f, 0u, 1u); // 命令をやり直すときにフラグへ戻す値
    s.io(undoS_);
    s.io(undoM_);
    s.io(splitDepth_);
    s.io(splitCycle_);
    s.io(splitAddr_);
    s.io(altAccess_);
    s.io(inAddrErr_);
    uint8_t hasFpu = fpu_ != nullptr;
    s.loc(hasFpu);
    if (hasFpu != (fpu_ != nullptr)) s.fail();
    else if (fpu_) fpu_->state(s);
    if (s.applying()) {
        // 速く読み書きするための表は作り直す (fast page は Machine::updateFastPages() が作る)。68060 の MMU が
        // 有効な間の TLB と ATC は、そのあとに stateTlb() で戻す
        updateMmu();
        flushTlb();
        attn_ = 1; // 次の命令の前に、停止・割り込み・トレースを調べる (cpu020.h の attn_)
    }
    // 68060 のサイクル数の数え方の状態 (同時実行、分岐の折りたたみ、分岐キャッシュ、命令の取り出しの位置) は、
    // stateTiming060() で読み書きする (版 10 から)。ここでは、読み込んだときに空にして、読んだ CACR でキャッシュが無効の
    // ときのバスの時間を決め直す。保存 (SAVE)、大きさを数える (SIZE)、並びの確認 (CHECK) では何も変えない
    // (保存しても、保存した側のそのあとのサイクル数は変わらない)
    if (s.applying()) {
        clearTiming060();
        updateCacheCosts();
    }
}

// 68060 のサイクル数の数え方の状態 (版 10 から。版 9 までは空から始める)。入れておかないと、保存したところから続けた
// ときと、読み込んで続けたときで、分岐の予測や同時実行の相手が違い、サイクル数が変わる。68030 では使わない値だが、
// 並びを同じにするために書く
void m68k::Cpu020::stateTiming060(x68k::StateIO& s) {
    s.tag(0x30365443); // "CT60"
    s.io(pairLead_);
    s.io(pairDest_);
    uint8_t foldOk = foldOk060_, fetchValid = fetchValid_;
    s.loc(foldOk);
    s.loc(fetchValid);
    if (s.applying()) {
        foldOk060_ = foldOk != 0;
        fetchValid_ = fetchValid != 0;
    }
    s.io(fetchLong_);
    s.io(bcachePc060_);
    s.io(bcacheState060_);
    if (s.applying()) {
        fetchLong_ &= ~3u;
        for (uint8_t& st : bcacheState060_) st &= (uint8_t)(BC_VALID | BC_USER | 3);
    }
}

// 68060 の MMU が有効な間の TLB と ATC (版 9 から)。表を引いた結果を覚えているもので、空にして始めると、
// 読んだあとに同じページを使うたびに表を引き直す (時間がかかり、PFLUSH せずに書き換えたデスクリプタが使われる) ので、
// 保存しなかったときと結果が変わる。TLB はホストのポインタを持つので、物理アドレスのページにして保存し、読むときに
// physPtr() でポインタに戻す (fast page を使うので、Machine::updateFastPages() のあとに呼ぶ)。
// MMU が無効なら (68030 はいつも)、TLB は結果を変えないので保存せず、空のままにする
void m68k::Cpu020::stateTlb(x68k::StateIO& s) {
    s.tag(0x544c4220); // "TLB "
    uint8_t keep = mmuOn_;
    s.loc(keep);
    if (keep > 1) s.fail();
    if (!keep) return;
    const bool apply = s.applying() && mmuOn_;
    for (int k = 0; k < 3; k++) {
        for (int sv = 0; sv < 2; sv++) {
            for (int i = 0; i < TLB_SIZE; i++) {
                TlbEntry& e = tlb_[k][sv][i];
                uint32_t tag = TLB_INVALID, pa = 0;
                if (s.saving() && e.tag != TLB_INVALID) {
                    // ホストのポインタから物理アドレスを求める (physPtr() の逆)。同じメモリが見える物理アドレスが
                    // いくつかあっても、どれに戻しても同じポインタになる
                    const uint8_t* p = (const uint8_t*)(e.base + ((uintptr_t)e.tag << 12));
                    bool found = false;
                    if (ext_ && p >= ext_ && (size_t)(p - ext_) < extSize_) {
                        pa = extStart_ + (uint32_t)(p - ext_);
                        found = true;
                    } else if (holeSize_ && (p == openBusRead_ || p == openBusWrite_)) {
                        // 何もつながっていない範囲のうち、setExtMemory() のメモリに重ならない番地にする
                        // (060turbo のローカルメモリは、範囲の先頭に SIMM がある)
                        pa = holeStart_;
                        if (pa - extStart_ < extSize_) pa = extStart_ + extSize_;
                        found = pa - holeStart_ < holeSize_;
                    } else {
                        for (int pg = 0; pg < FAST_PAGES && !found; pg++) {
                            const uint8_t* base = k == TLB_WRITE ? fastWrite_[sv][pg] : fastRead_[sv][pg];
                            if (base && p >= base && p - base < 0x10000) {
                                pa = (uint32_t)pg << 16 | (uint32_t)(p - base);
                                found = true;
                            }
                        }
                    }
                    // 求めた物理アドレスから同じポインタに戻せるものだけを入れる
                    if (found && physPtr(pa, k == TLB_WRITE, sv != 0) == p) tag = e.tag;
                }
                s.loc(tag);
                s.loc(pa);
                if (!apply) continue;
                e.tag = TLB_INVALID;
                if (tag > 0xfffffu || (tag & (TLB_SIZE - 1)) != (uint32_t)i || (pa & 0xfff)) continue;
                const uint8_t* p = physPtr(pa, k == TLB_WRITE, sv != 0);
                if (!p) continue;
                e.tag = tag;
                e.base = (uintptr_t)p - ((uintptr_t)tag << 12);
            }
        }
    }
    for (int f = 0; f < 2; f++) {
        for (int sv = 0; sv < 2; sv++) {
            for (int i = 0; i < ATC_SIZE; i++) {
                AtcEntry& e = atc_[f][sv][i];
                // 使っていない項目は tag だけが決まった値 (flushAtc())。残りは読まない (初期化していない bool を読まない)
                uint32_t tag = TLB_INVALID, ppage = 0;
                uint8_t writable = 0;
                if (s.saving() && e.tag != TLB_INVALID) {
                    tag = e.tag;
                    ppage = e.ppage;
                    writable = e.writable;
                }
                s.loc(tag);
                s.loc(ppage);
                s.loc(writable);
                if (!apply) continue;
                e.tag = TLB_INVALID;
                if (tag > 0xfffffu || (tag & (ATC_SIZE - 1)) != (uint32_t)i) continue;
                e.tag = tag;
                e.ppage = ppage & 0xfffffu;
                e.writable = writable != 0;
            }
        }
    }
}

void m68k::Fpu::state(x68k::StateIO& s) {
    s.tag(0x32383836); // "6882"
    s.io(fp_);
    s.io(fpcr_);
    s.io(fpsr_);
    s.io(fpiar_);
    s.io(nullState_);
    s.io(excPending_);
    s.io(pendingFrame_);
    s.io(excp060_);
    s.io(excpOp_);
    s.io(lastSrc_);
    s.io(frameSrc_);
    s.io(srcUnsupported_);
}
#endif

namespace x68k {

// ---------------------------------------------------------------- ポインタを持たない周辺機器

// 丸ごと読み書きするクラスは、bool のバイトと、添字やシフトの数に使う値を確かめてから戻す (rawChecked())
// 詰め物をメンバに置き換えたクラスは、大きさが前と同じことを確かめる (ステートのファイルの形を変えない)
void Mfp::state(StateIO& s) {
    static_assert(sizeof(Mfp) == 56, "layout of Mfp in the state");
    // プリスケーラの残りは分周比 (最大 200) より小さい。大きな値だと、そのぶんタイマを進める繰り返しが終わらない
    rawChecked(s, *this, [](const Mfp& t) {
        for (uint32_t p : t.prescale_)
            if (p > 255) return false;
        return boolOk(t.rxFull_) && t.txData_ >= -1 && t.txData_ <= 255;
    });
}
void Rtc::state(StateIO& s) { s.raw(*this); }
void SysPort::state(StateIO& s) { s.raw(*this); }
void Ioc::state(StateIO& s) { s.raw(*this); }
void Ppi::state(StateIO& s) { s.raw(*this); }
// 版 6 までは、アレイチェーンの BTC を、表を読んだときではなくブロックの終わりに減らしていた。ブロックの転送の途中
// (動いていて、最初の表を読み終えているチャネル) では、版 6 までの BTC は今の BTC より 1 大きい。
// 古い版を読むときは 1 減らし、古い版の並びで書くとき (試験用) は 1 増やして書く。
// 直さずに読むと、表を 1 つ余分に読んで転送する。
// 版 6 までの BTC が 0 のチャネル (版 6 までの数え方では途中のブロックで 0 にならないので、壊れたステートか、BTC を 0 で
// 始めたもの) は減らさない (減らすと $FFFF になり、表を 65535 個読みにいく)
void Dmac::state(StateIO& s) {
    const bool old = s.version() < 7;
    auto midChain = [this](int ch) { return active(ch) && (reg_[ch][OCR] & 0x0c) == 0x08 && !pendingChain_[ch]; };
    uint16_t saved[4];
    if (old && s.mode() == StateIO::SAVE) {
        for (int ch = 0; ch < 4; ch++) {
            saved[ch] = (uint16_t)get16(ch, BTC);
            if (midChain(ch)) set16(ch, BTC, (uint16_t)(saved[ch] + 1));
        }
    }
    rawChecked(s, *this, [](const Dmac& t) {
        return boolOk(t.pendingChain_) && boolOk(t.pendingError_) && boolOk(t.firstOperand_) && t.lastAck_ >= 0 &&
               t.lastAck_ <= 3 && t.lastRun_ >= 0 && t.lastRun_ <= 3;
    });
    if (old && s.mode() == StateIO::SAVE)
        for (int ch = 0; ch < 4; ch++) set16(ch, BTC, saved[ch]);
    if (old && s.applying() && s.ok())
        for (int ch = 0; ch < 4; ch++)
            if (midChain(ch) && get16(ch, BTC) != 0) set16(ch, BTC, get16(ch, BTC) - 1);
}
// OPM。レジスタから決まる値 (DT1、MUL、KS、レート、FB など) は、レジスタに書ける範囲になければ読まない
// (表の添字やシフトの数に使う)。エンベロープや出力の値は、取りうる範囲になければ読まない
void Opm::state(StateIO& s) {
    static_assert(sizeof(Opm) == 1892 && sizeof(Channel) == 196 && sizeof(Op) == 44, "layout of Opm in the state");
    rawChecked(s, *this, [](const Opm& t) {
        if (!boolOk(t.runA_) || !boolOk(t.runB_) || !boolOk(t.csmPending_) || !boolOk(t.lfoDirty_) || !boolOk(t.noiseBit_))
            return false;
        // タイマの残りと BUSY の残りは、書いたときに入れる値まで (大きな値だと、X68000 のソフトが待ち続ける)
        if (t.timerA_ > 1024 || t.timerB_ > 4096 || (t.runA_ && t.timerA_ == 0) || (t.runB_ && t.timerB_ == 0) ||
            t.busy_ > (uint32_t)CLOCKS_PER_SAMPLE)
            return false;
        if (t.lfoAm_ < 0 || t.lfoAm_ > 255 || t.lfoPm_ < -128 || t.lfoPm_ > 127 || t.lastPm_ < -128 || t.lastPm_ > 128 ||
            t.amd_ > 127 || t.pmd_ > 127 || t.egDivider_ < 0 || t.egDivider_ > 2)
            return false;
        for (const Channel& ch : t.ch_) {
            if (!boolOk(ch.left) || !boolOk(ch.right) || !boolOk(ch.pmActive) || !boolOk(ch.idle)) return false;
            if (ch.kc > 127 || ch.kf > 63 || ch.fb > 7 || ch.con > 7 || ch.pms > 7 || ch.ams > 3) return false;
            if (ch.fbOut[0] < -8192 || ch.fbOut[0] > 8192 || ch.fbOut[1] < -8192 || ch.fbOut[1] > 8192) return false;
            for (const Op& op : ch.op) {
                if (!boolOk(op.key) || !boolOk(op.amEnable)) return false;
                if (op.state > EG_RELEASE || op.dt1 > 7 || op.mul > 15 || op.tl > 127 || op.ks > 3 || op.ar > 31 ||
                    op.d1r > 31 || op.d2r > 31 || op.rr > 15 || op.d1l > 15 || op.dt2 > 3)
                    return false;
                for (uint8_t r : op.rate)
                    if (r > 63) return false;
                if (op.env < 0 || op.env > 1023 || op.d1lLevel < 0 || op.d1lLevel > 1023 || op.tlLevel < 0 ||
                    op.tlLevel > 1023 || op.phase > 0xfffff || op.step > 0x3fffff)
                    return false;
            }
        }
        return true;
    });
}
// 版 13 から、メンバを 1 つずつ書く (21 バイト。あとに 0 を 11 バイト足して、版 12 までと同じ 32 バイトにする)。
// 版 12 までは作り直す前のクラスを丸ごと (32 バイト) 書いていて、中身の並びが今のものと合わない。その中の
// 「再生中か」の 1 バイトだけを使い、ほかは読み捨てる。位置は、前の版を動かして、再生を始める前と後のステートを
// 比べて測った (前の版のソースは読んでいない)。再生中だったら playing_ だけを立てておき、Machine::state() が
// 発振と左右、周波数の選択を OPM と 8255 から決め直してから、再生を始め直す (波形は中心から)。
// 古い版の並びで書くとき (試験用) は、「再生中か」のほかは 0 を書く
void Adpcm::state(StateIO& s) {
    if (s.version() < STATE_REWRITTEN_ADPCM) {
        static constexpr int OLD_PLAYING = 0; // 32 バイトの中の「再生中か」の位置
        uint8_t old[32] = {};
        old[OLD_PLAYING] = playing_ ? 1 : 0;
        s.bytes(old, sizeof old);
        if (s.applying() && s.ok()) {
            reset();
            playing_ = old[OLD_PLAYING] == 1;
        }
        return;
    }
    s.io(playing_);
    s.io(clock4M_);
    s.io(drq_);
    s.io(latched_);
    s.io(second_);
    s.range(pan_, (uint8_t)0, (uint8_t)3);
    s.range(rate_, (uint8_t)0, (uint8_t)3);
    s.io(latch_);
    s.io(current_);
    s.range(signal_, -2048, 2047);
    s.range(step_, 0, STEP_MAX);
    s.range(elapsed_, 0u, 2047u); // period() (最大 2048) より小さい
    uint8_t spare[11] = {};
    s.bytes(spare, sizeof spare);
}
void Mercury::state(StateIO& s) {
    rawChecked(s, *this, [](const Mercury& t) {
        return boolOk(t.leftNext_) && t.head_ >= 0 && t.head_ < FIFO && t.count_ >= 0 && t.count_ <= FIFO &&
               t.outL_ >= -32768 && t.outL_ <= 32767 && t.outR_ >= -32768 && t.outR_ <= 32767; // 16 ビットの PCM
    });
}

// MIDI ボード。送り終えてまだ Sound に渡していないバイトも入る (ラスタごとに渡すので、ふつうは空)
void Ym3802::state(StateIO& s) {
    static_assert(sizeof(Ym3802) == 3080 && sizeof(MidiShadow) == 2896 && sizeof(platform::MidiStream) == 92,
                  "layout of Ym3802 in the state");
    s.raw(*this);
    if (!s.applying() && s.mode() != StateIO::SHIFT) return;
    if (s.applying()) sanitize();
    // 進め終えた時刻は CPU の時刻で持っている。CPU の時刻を戻したら、同じだけ戻す (StateIO::time() と同じ)
    if (s.timeShift()) synced_ = synced_ >= s.timeShift() ? synced_ - s.timeShift() : 0;
}

// ネットワークボード。EtherLink (ポインタ) と、まだ受け取っていないフレームは入れない
void Neptune::state(StateIO& s) {
    s.io(cr_);
    s.io(isr_);
    s.io(imr_);
    s.io(pstart_);
    s.io(pstop_);
    s.io(bnry_);
    s.io(curr_);
    s.io(tpsr_);
    s.io(tbcr_);
    s.io(rsar_);
    s.io(rbcr_);
    s.io(dmaAddr_);
    s.io(dmaCount_);
    s.io(rcr_);
    s.io(tcr_);
    s.io(dcr_);
    s.io(tsr_);
    s.io(rsr_);
    s.io(ncr_);
    s.io(cntr_);
    s.io(par_);
    s.io(mar_);
    s.io(rnpp_);
    s.io(lnpp_);
    s.io(dataLatch_);
    s.io(overflowRst_);
    s.io(prom_);
    s.memory(ram_, sizeof ram_);
    // 送っているフレームの残りの時間と、TXP を書いてからまだ poll() を呼んでいないか (版 11 から)。
    // 前の版は TXP を残さないので 0 にする
    if (s.version() >= 11) {
        s.range(txRemainNs_, 0u, transmitNs(0xffff));
        s.io(txFirstPoll_);
    }
    if (s.applying()) {
        // 残りは今の TBCR では切り詰めない (送っている間に TBCR を書き換えても、送っているフレームの長さは変わらない)。
        // 上限は TBCR の最大値の時間 (上の range())
        if (s.version() < 11 || !(cr_ & CR_TXP)) {
            txRemainNs_ = 0;
            txFirstPoll_ = false;
        }
    }
}

// 数値演算プロセッサボード。FpuCore は計算の時間を足す先 (ポインタ) を持つので、レジスタだけを入れる
#ifndef PIX68K_NO_FPU_BOARD
void FpuBoard::state(StateIO& s) {
    s.tag(0x36383831); // "6881"
    s.io(core_.fp_);
    s.io(core_.fpcr_);
    s.io(core_.fpsr_);
    s.io(core_.fpiar_);
    s.io(nullState_);
    s.io(excPending_);
    s.io(excReported_);
    s.io(pendFrame_);
    s.io(resp_);
    s.io(next_);
    s.io(next2_);
    s.io(cmd_);
    const uint8_t phase = s.range(phase_, (uint8_t)P_IDLE, (uint8_t)P_RESTORE);
    s.range(inKind_, (uint8_t)IN_SOURCE, (uint8_t)IN_MOVEM);
    s.io(list_);
    s.io(outDone_);
    s.io(restoreEcho_);
    // buf_ の添字と、受け渡す長さ。オペランドの受け渡し (P_IN、P_OUT、P_MAINREG) は 96 バイトまで (FMOVEM の 8 個。
    // 受け取ったあと 24 語の配列に直す)、状態のフレーム (P_SAVE、P_RESTORE) は IDLE か BUSY の長さだけ
    const int len = s.range(len_, 0, (int)sizeof buf_);
    s.range(pos_, 0, len);
    if (!s.saving()) {
        const bool frame = phase == P_SAVE || phase == P_RESTORE;
        if ((phase == P_IN || phase == P_OUT || phase == P_MAINREG) && len > 96) s.fail();
        if (frame && len != 24 && len != (int)sizeof buf_) s.fail();
    }
    s.io(fpiarHi_);
    s.io(buf_);
    // 受け渡し終えているのに受け渡しの途中、という状態にはしない (buf_[pos_] を読み書きしてから数を比べる)
    if (s.applying() && phase_ != P_IDLE && phase_ != P_REGSEL && pos_ >= len_) phase_ = P_IDLE;
}

#endif

// ---------------------------------------------------------------- 仮想関数やポインタを持つもの

void Keyboard::state(StateIO& s) {
    s.tag(0x4b455942); // "KEYB"
    s.io(queue_);
    s.range(wp_, 0u, (uint32_t)QUEUE_SIZE - 1);
    s.range(rp_, 0u, (uint32_t)QUEUE_SIZE - 1);
    s.io(hidDown_);
    s.io(count_);
    s.io(pendingBreak_);
    s.io(anyPending_);
    s.io(enabled_);
    s.io(leds_);
    s.io(delayMs_);
    s.io(intervalMs_);
    s.io(repeatCode_);
    s.time(nextRepeat_); // CPU の時刻で持っている値
    s.time(nextSend_);
    s.time(lastNow_);
}

void Mouse::state(StateIO& s) {
    s.tag(0x4d4f5553); // "MOUS"
    // 溜めた移動量は ±(1 << 20) に抑えてある (Mouse::onMouse()。超えていると、次に足したときにあふれる)
    s.range(dx_, -(1 << 20), 1 << 20);
    s.range(dy_, -(1 << 20), 1 << 20);
    s.io(buttons_);
    s.io(latched_);
    s.io(packet_);
    s.range(sent_, 0, 3);
    s.time(nextByte_); // CPU の時刻で持っている値
    s.time(lastNow_);
    s.io(packets_);
}

void Scc::state(StateIO& s) {
    s.tag(0x53434320); // "SCC "
    s.io(wr_);
    if (s.version() < STATE_SCC_RS232C) {
        // 版 13 までの並び: チャネルごとの構造体 (受信の FIFO、要求、レジスタの選択のポインタ) をそのまま置いた
        struct OldChan {
            uint8_t fifo[FIFO_SIZE];
            uint8_t pad; // 詰め物 (0 で書く。x68k/state.h)
            int count;
            bool overrun, rxFirstArmed, rxFirstIp, txIp;
            int pointer;
        };
        static_assert(sizeof(OldChan) == 16, "layout of the old Scc::Chan in the state");
        int ptr = 0;
        for (int ch = 0; ch < 2; ch++) {
            Chan& c = chan_[ch];
            OldChan o = {};
            memcpy(o.fifo, c.fifo, sizeof o.fifo);
            o.count = c.count;
            o.overrun = c.overrun;
            o.rxFirstArmed = c.rxFirstArmed;
            o.rxFirstIp = c.rxFirstIp;
            o.txIp = c.txIp;
            o.pointer = pointer_;
            rawChecked(s, o, [](const OldChan& t) {
                return t.count >= 0 && t.count <= FIFO_SIZE && boolOk(t.overrun) && boolOk(t.rxFirstArmed) &&
                       boolOk(t.rxFirstIp) && boolOk(t.txIp) && t.pointer >= 0 && t.pointer <= 15;
            });
            if (s.applying()) {
                c = Chan();
                memcpy(c.fifo, o.fifo, sizeof o.fifo);
                c.count = o.count;
                c.overrun = o.overrun;
                c.rxFirstArmed = o.rxFirstArmed;
                c.rxFirstIp = o.rxFirstIp;
                c.txIp = o.txIp;
                if (o.pointer) ptr = o.pointer; // ポインタは 2 つのチャネルで 1 つになった。選んでいたほうを使う
            }
        }
        s.io(ius_);
        if (s.applying()) pointer_ = ptr;
        return;
    }
    for (Chan& c : chan_) {
        s.io(c.fifo);
        for (bool& e : c.fifoErr) s.io(e);
        s.range(c.count, 0, FIFO_SIZE);
        s.io(c.overrun);
        s.io(c.rxLocked);
        s.io(c.rxLockedRead);
        s.io(c.rxFirstArmed);
        s.io(c.rxFirstIp);
        s.io(c.txIp);
        s.io(c.extIp);
        s.io(c.extLatched);
        s.io(c.extLatch);
        s.io(c.lines);
        s.io(c.txBufFull);
        s.io(c.txBuf);
        s.io(c.shiftBusy);
        s.io(c.shiftByte);
        s.time(c.shiftEnd);
        s.time(c.rxNext);
    }
    s.range(pointer_, 0, 15);
    s.io(ius_);
    s.time(now_);
}

void Fdc::state(StateIO& s) {
    s.tag(0x46444320); // "FDC "
    s.io(ejectReq_);
    s.io(ejectMask_);
    s.range(access_, -1, DRIVES - 1);
    s.en(phase_, RESULT + 1);
    s.io(cmd_);
    // cmd_ と result_ の添字。コマンドは cmd_[cmdLen_] に入れてから数を比べ、結果は result_[resultPos_] を読んでから比べる
    s.range(cmdLen_, 0, (int)sizeof cmd_ - 1);
    s.range(cmdNeed_, 0, (int)sizeof cmd_);
    s.io(result_);
    s.range(resultLen_, 0, (int)sizeof result_);
    s.range(resultPos_, 0, (int)sizeof result_);
    s.io(st0_);
    s.io(st1_);
    s.io(st2_);
    s.io(seekSt0_);
    s.io(cylinder_);
    s.io(idIndex_);
    s.io(irq_);
    s.io(forceReady_);
    s.io(nonDma_);
    s.io(driveCtrl_);
    const int bufLen = s.range(bufLen_, 0, (int)sizeof buf_);
    s.range(bufPos_, 0, bufLen);
    s.io(sectorDone_);
    s.io(lastSector_);
    s.io(buf_);
    s.io(scanBuf_);
    // 転送中のセクタ: 保存したときのドライブ (US) のイメージの大きさと、セクタの表の番号
    int32_t index = -1;
    uint32_t imageSize = 0;
    if (s.saving()) {
        const FloppyImage* img = drive_[us()];
        if (img && img->loaded()) {
            index = img->sectorIndex(sector_);
            imageSize = (uint32_t)img->size();
        }
    }
    s.loc(index);
    s.loc(imageSize);
    if (!s.applying()) return;
    // ドライブのイメージは今のものを使う (inserted_ も今のまま)
    sector_ = nullptr;
    const FloppyImage* img = drive_[us()];
    if (index >= 0 && img && img->loaded() && img->size() == imageSize) sector_ = img->sectorAt(index);
    // セクタより長いデータは書き戻せない (commitWrite() はセクタに bufLen_ バイトを写す)
    if (sector_ && (uint32_t)bufLen_ > sector_->size) sector_ = nullptr;
    // 結果を読み終えているのに結果のフェーズ、という状態にはしない (result_[resultPos_] を読んでから数を比べる)
    if (phase_ == RESULT && resultPos_ >= resultLen_) phase_ = COMMAND;
    if (!sector_ && (phase_ == EXEC_READ || phase_ == EXEC_WRITE || phase_ == EXEC_WRITE_ID) && index >= 0) {
        // 保存したときと違うディスク: 転送の途中の状態は捨てる
        phase_ = COMMAND;
        cmdLen_ = 0;
    }
}

namespace {
// 古い版の、読まない部分 (n バイト)。読むときは捨て、書くときは 0 を書く
void skipOld(StateIO& s, size_t n) {
    uint8_t zero[64];
    while (n > 0) {
        const size_t k = n < sizeof zero ? n : sizeof zero;
        memset(zero, 0, k);
        s.local(zero, k);
        n -= k;
    }
}
} // namespace

// SASI: バスの段階、コマンド、センスデータ、データの位置。イメージとの受け渡しの位置 (DiskCursor) は、イメージの中の
// 番地で持ち、読んだあとに作り直す。保存したときとイメージの大きさが違えば、コマンドを捨ててバスフリーにする
void Sasi::state(StateIO& s) {
    s.tag(0x53415349); // "SASI"
    if (s.version() < STATE_REWRITTEN_SCSI) {
        // 版 11 までの並びは読まない: 長さ (区切りの 4 バイトを含めて 123 バイト。前の実装を動かして測った) だけ読み飛ばして、
        // バスフリーから始める。古い版で書くとき (試験用) は、同じ長さの 0 を書く
        skipOld(s, 123 - 4);
        if (s.applying() && s.ok()) reset();
        return;
    }
    uint8_t stage = stage_;
    s.loc(stage);
    if (!s.saving() && stage >= STAGE_COUNT) s.fail();
    if (s.applying() && s.ok()) stage_ = (Stage)stage;
    s.io(interrupt_);
    uint8_t fromDiskByte = fromDisk_;
    const bool fromDisk = s.range(fromDiskByte, (uint8_t)0, (uint8_t)1) != 0;
    s.range(id_, (uint8_t)0, (uint8_t)7);
    s.io(cmd_);
    // コマンドフェーズでは cmd_[cmdLen_] に入れてから数を比べる
    s.range(cmdLen_, (uint8_t)0, (uint8_t)(stage == COMMAND ? sizeof cmd_ - 1 : sizeof cmd_));
    s.io(status_);
    s.io(sense_);
    s.io(buf_);
    const uint32_t len = s.range(len_, 0u, 0xffffffffu);
    const uint32_t pos = s.range(pos_, 0u, 0xffffffffu);
    const bool data = stage == DATA_IN || stage == DATA_OUT || stage == WORKING;
    // buf_ を相手にするデータは、buf_ に収まること (FORMAT の間はいつもイメージが相手)
    if (!s.saving() && data && (pos >= len || (!fromDisk && (len > sizeof buf_ || stage == WORKING)))) s.fail();
    uint64_t base = 0, imageBytes = 0;
    if (s.saving() && data && fromDisk_) {
        base = cursor_.base();
        const HardDiskImage* img = image_[id_ * 2 + lun()];
        imageBytes = img ? img->bytes() : 0;
    }
    s.loc(base);
    s.loc(imageBytes);
    if (!s.applying() || !s.ok()) return;
    fromDisk_ = fromDisk;
    cursor_.stop();
    if (data && fromDisk_) {
        HardDiskImage* img = drive();
        if (!img || img->bytes() != imageBytes || base > imageBytes || len_ > imageBytes - base) {
            busFree(); // 保存したときと違うイメージ
            interrupt_ = false;
        } else {
            cursor_.start(img, base, len_);
        }
    }
}

// SPC: レジスタ、FIFO、ハード転送の状態と、バスの向こうのターゲットの状態 (フェーズ、コマンド、データの位置、センスデータ)。
// イメージとの受け渡しの位置 (DiskCursor) は、イメージの中の番地とセクタの並びで持ち、読んだあとに作り直す。
// 保存したときと装置やイメージが違えば、コマンドを捨ててバスフリーにする。最後に CD-ROM 装置の状態 (あれば)
void Spc::state(StateIO& s) {
    s.tag(0x53504320); // "SPC "
    if (s.version() < STATE_REWRITTEN_SCSI) {
        // 版 11 までの並び。前の実装を動かして、長さと、レジスタの値が入る位置だけを測った (中身は読んでいない):
        // 区切りの 4 バイトを含めて、版 3 までは 472 バイト、版 4 からは 473 バイト、版 8 からは 481 バイト。区切りのすぐあとの 16 バイトに、
        // BDID (ID のビット)、SCTL、SCMD、(読まない 2 バイト)、PCTL、(1 バイト)、TEMP、(4 バイト)、転送カウンタ (32 ビット)。
        // そのあとの 3 バイトを置いて、結合しているターゲットの ID (32 ビット。バスフリーなら -1)。
        // レジスタだけを戻し (SCSI のドライバが初期化したままにする)、バスと転送の状態は読み飛ばしてバスフリーから始める。
        // ターゲットと結合している途中 (SCSI のコマンドの途中) で保存したステートは読まない: コマンドの続きを戻せず、
        // ドライバが答えを待ち続けて止まる。
        // 古い版で書くとき (試験用) は、バスフリーの形にして、レジスタと ID のほかは 0 を書く。
        // 版 8 からは、そのあとに CD-ROM 装置の状態 (1050 バイト。CdDrive::state()) が、装置がなくても入っている
        uint8_t reg[23] = {};
        if (s.saving()) {
            memset(reg + 19, 0xff, 4);
            reg[0] = (uint8_t)(1u << bdid_);
            reg[1] = sctl_;
            reg[2] = scmd_;
            reg[5] = pctl_;
            reg[7] = temp_;
            reg[12] = (uint8_t)transferCount_;
            reg[13] = (uint8_t)(transferCount_ >> 8);
            reg[14] = (uint8_t)(transferCount_ >> 16);
        }
        s.local(reg, sizeof reg);
        if (!s.saving() && (reg[19] & reg[20] & reg[21] & reg[22]) != 0xff) {
            s.fail("saved in the middle of a SCSI command by an older version");
        }
        skipOld(s, (s.version() >= 8 ? 481u : s.version() >= 4 ? 473u : 472u) - 4 - sizeof reg);
        if (s.applying() && s.ok()) {
            reset();
            for (int i = 0; i < 8; i++)
                if (reg[0] & (1u << i)) bdid_ = (uint8_t)i;
            sctl_ = reg[1];
            scmd_ = reg[2];
            pctl_ = reg[5] & (PCTL_BUSFREE_INT | 7);
            temp_ = reg[7];
            transferCount_ = (uint32_t)reg[12] | (uint32_t)reg[13] << 8 | (uint32_t)reg[14] << 16;
        }
        if (s.version() >= 8) {
            if (cd_) {
                cd_->state(s);
            } else {
                CdDrive* scratch = new CdDrive;
                if (scratch) scratch->state(s);
                else s.fail();
                delete scratch;
            }
        }
        return;
    }
    s.range(bdid_, (uint8_t)0, (uint8_t)7);
    s.io(sctl_);
    s.io(scmd_);
    s.io(ints_);
    s.io(sdgc_);
    s.io(pctl_);
    s.io(temp_);
    s.range(fifoCount_, (uint8_t)0, (uint8_t)FIFO_SIZE);
    s.io(endInts_);
    s.io(fifo_);
    s.range(transferCount_, 0u, 0xffffffu);
    s.io(connected_);
    s.io(xfer_);
    s.io(fifoIn_);
    s.io(pad_);
    s.io(ack_);
    s.io(ackValid_);
    s.io(atnOut_);
    s.io(selWait_);
    s.range(target_, (uint8_t)0, (uint8_t)7);
    const uint8_t phase = s.range(tphase_, (uint8_t)0, (uint8_t)7);
    if (!s.saving() && (phase == 4 || phase == 5)) s.fail(); // ない組み合わせ
    s.range(lun_, (uint8_t)0, (uint8_t)7);
    s.io(cdb_);
    s.range(cdbLen_, (uint8_t)0, (uint8_t)sizeof cdb_);
    s.range(msgExt_, (uint16_t)0, (uint16_t)257);
    s.io(msgThenFree_);
    s.io(identified_);
    s.io(status_);
    s.io(msgIn_);
    s.io(ackByte_);
    uint8_t srcByte = src_;
    const uint8_t src = s.range(srcByte, (uint8_t)0, (uint8_t)(SRC_COUNT - 1));
    s.io(writing_);
    const uint32_t len = s.range(len_, 0u, 0xffffffffu);
    s.range(pos_, 0u, 0xffffffffu);
    // 自分のバッファを相手にするデータは、その大きさに収まること。データアウト (MODE SELECT のパラメータ) は
    // 入らない分を捨てるので、CDB で言える長さ (16 ビット) まで
    if (!s.saving() && ((src == SRC_BUF && len > (phase == PH_DATA_OUT ? 0xffffu : sizeof buf_)) ||
                        (src == SRC_CD && len > CdDrive::BUF_SIZE))) {
        s.fail();
    }
    s.io(sense_);
    s.io(buf_);
    uint64_t base = 0, imageBytes = 0;
    uint32_t stride = 0, payload = 0;
    if (s.saving() && src_ == SRC_DISK) {
        base = cursor_.base();
        stride = cursor_.stride();
        payload = cursor_.payload();
        const HardDiskImage* img = writing_ ? disk_[target_] : image(target_);
        imageBytes = img ? img->bytes() : 0;
    }
    s.loc(base);
    s.loc(stride);
    s.loc(payload);
    s.loc(imageBytes);
    uint8_t cdTarget = connected_ && isCdrom(target_); // 結合している装置が CD-ROM 装置か
    cdTarget = s.range(cdTarget, (uint8_t)0, (uint8_t)1);
    if (s.applying() && s.ok()) {
        src_ = (Source)src;
        cursor_.stop();
        if (!connected_) {
            dropBus();
        } else {
            const bool data = tphase_ == PH_DATA_IN || tphase_ == PH_DATA_OUT;
            // 保存したときと違う種類の装置につないでいたら、コマンドは捨てる
            bool keep = present(target_) && (cdTarget != 0) == isCdrom(target_);
            if (data && (src_ == SRC_NONE || pos_ >= len_)) keep = false;
            if (!data && src_ != SRC_NONE) keep = false;
            if (src_ == SRC_CD && !isCdrom(target_)) keep = false;
            if (keep && src_ == SRC_DISK) {
                // 書き込みの先はハードディスクだけ。読み出しは、ハードディスクか CD-ROM 装置の媒体
                HardDiskImage* img = writing_ || tphase_ == PH_DATA_OUT ? disk_[target_] : image(target_);
                // 転送の最後のバイトの、イメージの中の位置
                const uint64_t last = stride && payload ? (uint64_t)((len_ - 1) / payload) * stride + (len_ - 1) % payload : len_ - 1;
                // セクタの並びは、CD-ROM の読み出しが作るものだけ (2048 バイトのユーザデータが、それより大きいセクタに入っている)。
                // DiskCursor は 16 ビットで持つ
                const bool layoutOk = stride == 0 ? payload == 0 : payload == 2048 && stride > 2048 && stride <= 0xffff && !writing_;
                if (!img || !layoutOk || writing_ != (tphase_ == PH_DATA_OUT) || img->bytes() != imageBytes ||
                    base >= imageBytes || last >= imageBytes - base) {
                    keep = false; // 保存したときと違うイメージ
                } else {
                    cursor_.start(img, base, len_, stride, payload);
                }
            }
            if (!keep) {
                dropBus();
                fifoCount_ = 0; // 捨てたコマンドのデータを残さない
                endInts_ = 0;
            }
        }
    }
    // CD-ROM 装置の状態。保存したときにあって今はなければ読み捨て、今だけあれば再生を止める
    uint8_t hasCd = cd_ != nullptr;
    hasCd = s.range(hasCd, (uint8_t)0, (uint8_t)1);
    if (hasCd) {
        if (cd_) {
            cd_->state(s);
        } else {
            CdDrive* scratch = new CdDrive;
            if (scratch) scratch->state(s);
            else s.fail();
            delete scratch;
        }
    } else if (s.applying() && s.ok() && cd_) {
        cd_->stop();
        cd_->setPrevent(false);
    }
}

void CdDrive::state(StateIO& s) {
    s.tag(0x43442020); // "CD  "
    s.io(trayOpen_);
    s.io(prevent_);
    s.io(attention_);
    s.io(audio_);
    s.io(deferKey_);
    s.io(deferAsc_);
    s.io(pos_);
    s.io(end_);
    s.io(frame_);
    s.range(acc_, 0u, 39999u); // クロックの端数 (40000 で割った余り。大きな値だと、そのぶんフレームを作る)
    s.io(buf_);
    if (!s.applying()) return;
    // 読んでいる位置 (cursor_) とまだ取り出していないフレームは入れない: 次に進めたときに位置から作り直す
    owed_ = 0;
    cursor_.stop();
    runLba_ = runEnd_ = 0;
    // CdMixer はそのまま (音の合成を別のコアで行うときの読み手の CdMixer と、記録の差のもとを合わせたままにする)
    if (frame_ >= FRAMES_PER_SECTOR) frame_ = 0;
    // 媒体は環境のもの。保存したときと違って再生の範囲が今の媒体に合わなければ、再生を止める
    if ((audio_ == AUDIO_PLAY || audio_ == AUDIO_PAUSED) && (!ready() || pos_ >= toc_.leadOut || end_ > toc_.leadOut)) stop();
    if (!image_) attention_ = false;
}

// ---------------------------------------------------------------- 音

void Sound::state(StateIO& s) {
    s.tag(0x534e4420); // "SND "
    opm_.state(s);
    adpcm_.state(s);
    // 出力の周波数、補間の位置、たまり具合の調整、リング、記録の設定は環境のものなので入れない
    s.range(sub_, 0u, (uint32_t)Opm::CLOCKS_PER_SAMPLE - 1); // 大きな値だと、そのぶんサンプルを作る繰り返しが終わらない
    // フィルタの値は、取りうる範囲になければ読まない (大きな値だと、Sound::output() の引き算と掛け算があふれる)。
    // フィルタに入る値は、OPM (8 チャネル x 4 オペレータ x 8191 の 1/2 = 131056)、ADPCM (12 ビットの値 x 8 = 16384)、
    // まーきゅりーゆにっと (16384) の和で、
    // 絶対値は 2^20 より小さい。直流を切るフィルタの出力はその 2 倍まで、低域通過フィルタの値はそれを超えない。
    // 範囲はそれぞれ 2 倍の余裕を取る: 前の入力 ±2^20、直流を切るフィルタの出力 (2^20 倍の値) ±2^41、
    // 低域通過フィルタ ±2^22、補間の前のサンプルは 16 ビット。この範囲なら、出力の式の 64 ビットの掛け算は 2^62 を超えない
    for (int32_t& v : lp1_) s.range(v, -(1 << 22), 1 << 22);
    for (int32_t& v : lp2_) s.range(v, -(1 << 22), 1 << 22);
    for (int64_t& v : hpOut_) s.range(v, -((int64_t)1 << 41), (int64_t)1 << 41);
    for (int32_t& v : hpIn_) s.range(v, -(1 << 20), 1 << 20);
    for (int32_t& v : prev_) s.range(v, -32768, 32767);
    // 最後に記録した J_LINES の値は、読んだ値と合わない。次の setAdpcmLines() を必ず記録するようにする
    // (しないと、読む前と同じ値に戻したときに記録されず、合成するコアの値がステートの値のまま残る)
    if (s.applying()) lines_ = ~0u;
}

void Sound::copyStateFrom(const Sound& from) {
    opm_ = from.opm_;
    adpcm_ = from.adpcm_;
    sub_ = from.sub_;
    // フィルタの状態も、写す元が音を作っているかに関係なく写す (ステートを読んだ Machine の Sound は音を作って
    // いないが、フィルタの状態はファイルから読んだものを持っている)
    memcpy(lp1_, from.lp1_, sizeof lp1_);
    memcpy(lp2_, from.lp2_, sizeof lp2_);
    memcpy(hpOut_, from.hpOut_, sizeof hpOut_);
    memcpy(hpIn_, from.hpIn_, sizeof hpIn_);
    memcpy(prev_, from.prev_, sizeof prev_);
}

void copySoundState(const Sound& from, Sound& to) { to.copyStateFrom(from); }

// ---------------------------------------------------------------- 画面

void Video::state(StateIO& s) {
    s.tag(0x56494400); // "VID"
    s.io(crtc_);
    s.io(operation_);
    s.io(fastClearRequested_);
    s.range(fastClearFields_, 0, 2); // 高速クリアが終わるまでの垂直表示期間の数 (1、インタレースは 2 から数える)
    s.memory(tvram_, sizeof tvram_);
    s.memory((uint8_t*)gvram_, sizeof gvram_);
    s.io(palette_);
    s.memory(sprite_, sizeof sprite_);
    colorTablesState(s);
    // CRTC の世代は保存しない。読んだときは、世代で覚えている計算 (Machine と Renderer) をやり直させるために進める
    if (s.applying()) { crtcWrites_ += 1; spriteRowsWrites_ += 1; }
}

// 色の表は palette_ から作れるが、前の版のステートと並びを合わせるために 4 つとも書く。
// 読んだときは、読んだ値を使わずに palette_ から作り直す (同じ値になる)
void Video::colorTablesState(StateIO& s) {
    auto each = [&s](auto&... table) { (s.io(table), ...); };
    each(colors_.word, colors_.xrgb, colors_.high, colors_.low);
    if (s.applying()) rebuildPaletteTables();
}

void Video::copyStateFrom(const Video& from) {
    VideoJournal* j = journal_;
    const uint32_t nextRows = spriteRowsWrites_ + 1;
    *this = from;
    journal_ = j;
    spriteRowsWrites_ = nextRows;
}

void copyVideoState(const Video& from, Video& to) { to.copyStateFrom(from); }

// ---------------------------------------------------------------- Machine

void Machine::state(StateIO& s) {
    s.tag(0x4d414348); // "MACH"
    cpu_.state(s);
#ifndef PIX68K_NO_CPU020
    if (cpu020_) cpu020_->state(s);
#endif
    s.tag(0x4d454d20); // "MEM "
    s.memory(ram_, config_.ramSize);
#ifndef PIX68K_NO_CPU020
    if (highMem_) s.memory(highMem_, config_.highMemSize);
    if (localMem_) s.memory(localMem_, config_.localMemSize);
#endif
    s.io(sram_);
    s.io(sramInitPending_);
    s.io(sramInitWritten_);
    s.io(areaSet_);
    video_->state(s);
    mfp_.state(s);
    keyboard_.state(s);
    rtc_.state(s);
    sysPort_.state(s);
    ioc_.state(s);
    fdc_.state(s);
    sound_.state(s);
    dmac_.state(s);
    s.io(inDma_);
    scc_.state(s);
    mouse_.state(s);
    ppi_.state(s);
    sasi_.state(s);
    spc_.state(s);
    spcEx_.state(s); // 外付け SCSI ボード (ボードの有無はヘッダで確かめている。なければ使わないまま)
    // 数値演算プロセッサボードとまーきゅりーゆにっと (つないでいるときだけ。有無はヘッダで確かめている)
#ifndef PIX68K_NO_FPU_BOARD
    if (fpuBoards_[0]) {
        s.tag(0x46505542); // "FPUB"
        fpuBoards_[0]->state(s);
    }
    // 2 枚目の数値演算プロセッサボード (版 16 から。版 15 までのファイルにはない。ヘッダの bit4 で確かめている)
    if (fpuBoards_[1]) {
        s.tag(0x46505532); // "FPU2"
        fpuBoards_[1]->state(s);
    }
#endif
    if (mercury_) {
        s.tag(0x4d455243); // "MERC"
        mercury_->state(s);
    }
    if (ts6bga_) {
        s.tag(0x54533642); // TS6B (version 17)
        ts6bga_->state(s);
    }
    if (neptune_) {
        s.tag(0x4e455054); // "NEPT"
        neptune_->state(s);
    }
    if (midi_) {
        s.tag(0x4d494449); // "MIDI"
        midi_->state(s);
    }
    s.tag(0x54494d45); // "TIME"
    // 端数は、取りうる範囲になければ読まない (大きな値だと、そのぶん周辺機器を進める繰り返しが終わらない)
    s.time(synced_);
    s.range(mfpAcc_, (uint64_t)0, (uint64_t)mfpDiv_);
    s.range(secondAcc_, (uint64_t)0, (uint64_t)config_.cpuHz);
    s.time(lineStart_);
    s.time(nextLine_);
    s.io(lineCycles_);
    s.range(lineRem_, (uint64_t)0, (uint64_t)0xffffffffu); // ドットクロックで割った余り (次のラスタの長さに足す)
    s.range(hphase_, 0, 2);
    s.range(line_, 0, 0xffff);
    s.io(frames_);
    // 68000 の速さを変えて動かしたときの時間の端数 (版 5 から。版 4 までは 0)
    if (s.version() >= 5) {
        s.range(speedFrac_, (uint64_t)0, (uint64_t)(1u << SPEED_FRAC_BITS) - 1);
    } else if (s.applying()) {
        speedFrac_ = 0;
    }
    // 68000 のバスのウェイトと DRAM リフレッシュの端数 (版 6 から。版 5 までは 0 から始める)
    if (s.version() >= 6) {
        s.range(waitFrac_, 0u, 3u);
        s.range(refreshAcc_, 0u, REFRESH_PERIOD - 1);
    } else if (s.applying()) {
        waitFrac_ = 0;
        refreshAcc_ = 0;
    }
    // CPU が直接読み書きするページを、読んだ状態 (エリアセットなど) で決め直す。68030 / 68060 の TLB も空になる
    if (s.applying()) updateFastPages();
#ifndef PIX68K_NO_CPU020
    // 68060 の MMU が有効な間の TLB と ATC (版 9 から。版 8 までは空から始める)。fast page を決め直したあとに読む
    if (cpu020_ && s.version() >= 9) cpu020_->stateTlb(s);
    // 68060 のサイクル数の数え方の状態 (版 10 から。版 9 までは、Cpu020::state() が空にしたまま)
    if (cpu020_ && s.version() >= 10) cpu020_->stateTiming060(s);
#endif
    s.tag(0x454e4421); // "END!"
    if (s.applying()) {
        // 時刻 (CPU のサイクル数) は、CPU の今のサイクル数から離れすぎていれば今に合わせる (離れていると、追いつくまで
        // 周辺機器やラスタを進める繰り返しが終わらない)。ふつうのステートでは、どれも 1 ラスタの長さより近い
        const uint64_t now = cycles(), far = config_.cpuHz / 10;
        if (synced_ > now || now - synced_ > far) synced_ = now;
        // (電源を入れる前に保存したステートは、ラスタの長さが 0 のまま)
        const bool beforePowerOn = lineCycles_ == 0 && lineStart_ == 0 && nextLine_ == 0;
        if (!beforePowerOn && (lineCycles_ == 0 || lineCycles_ > far || nextLine_ - lineStart_ != lineCycles_ ||
                               lineStart_ > now + far || now > nextLine_ + far)) {
            lineStart_ = now;
            lineRem_ = 0;
            computeLine();
        }
        if (midi_) midi_->limitLag(now, far, config_.cpuHz);
        hdLineStart_ = ~0ull; // horizontalDisplayTimes() を計算し直させる
        // 版 12 までの ADPCM の状態は、「再生中か」だけを読んだ (Adpcm::state())。発振と左右、周波数の選択を OPM と
        // 8255 から決め直す。再生の途中で保存したファイルなら、再生を始め直す (波形は中心から。止めたままだと DMAC の
        // チャネル 3 の転送が終わらず、終わりを待つソフトウェアが進まない)
        if (s.version() < STATE_REWRITTEN_ADPCM) {
            sound_.setAdpcmLines(sound_.opm().ct2(), ppi_.adpcmPan(), ppi_.adpcmRate());
            if (sound_.adpcm().playing()) sound_.adpcmWrite(1, 0x02);
        }
        waitCycles_ = 0;      // CPU を動かしている区切りの中だけで使う値 (Machine::refreshWait())
        sramDirty_ = true;    // 読んだ SRAM をファイルに書き戻させる
        // 音源の中の状態はステートに入れない。鳴っている音を止め、ステートに入っている音源の設定 (MIDI ボードの
        // MidiShadow。プログラム、コントロールチェンジ、ランニングステータスなど) を音源に送り直す
        if (midi_) resyncMidi(midiOut());
    }
}

// 時刻を cycles サイクル戻す (machine.h)。CPU の時刻は直接、周辺機器の時刻は state() を SHIFT で通して、
// ステートを読むときに時刻を戻すのと同じ値 (StateIO::time()、Ym3802 の進め終えた時刻) を同じだけ戻す
void Machine::shiftTime(uint64_t cycles) {
    if (!cycles || cycles > this->cycles()) return;
#ifndef PIX68K_NO_CPU020
    if (use020()) cpu020_->shiftTime(cycles);
    else
#endif
        cpu_.setCycles(cpu_.cycles() - cycles);
    StateIO s(StateIO::SHIFT, nullptr, 0);
    s.setTimeShift(cycles);
    state(s);
    hdLineStart_ = ~0ull; // horizontalDisplayTimes() を計算し直させる (ラスタの始まりの時刻で覚えている)
    timeShifted_ += cycles;
}

void Machine::rebaseTime() {
    const uint64_t now = cycles();
#ifndef PIX68K_NO_CPU020
    const uint64_t max = use020() ? STATE_CPU020_CYCLES_MAX : STATE_CPU_CYCLES_MAX;
#else
    const uint64_t max = STATE_CPU_CYCLES_MAX;
#endif
    if (now <= max) return;
    shiftTime(now - stateKeptTime(now, max, STATE_CPU_TIME_PAST));
}

// ---------------------------------------------------------------- 入口

size_t stateSize(Machine& m) {
    StateIO s(StateIO::SIZE, nullptr, 0);
    m.state(s);
    return STATE_HEADER_SIZE + s.pos();
}

size_t stateSizeBound(Machine& m) {
    StateIO s(StateIO::SIZE, nullptr, 0);
    s.countFullMemory();
    m.state(s);
    return STATE_HEADER_SIZE + s.pos();
}

// つないでいる拡張ボード (ヘッダの +60)
static uint32_t boardBits(Machine& m) {
    return (m.fpuBoard() ? 1u : 0u) | (m.mercury() ? 2u : 0u) | (m.neptune() ? 4u : 0u) | (m.midiBoard() ? 8u : 0u) |
           (m.fpuBoard(1) ? 16u : 0u) | (m.ts6bga() ? 32u : 0u) |
           (m.ts6bga() && m.ts6bga()->window(0xef0000) ? 64u : 0u);
}

bool saveState(Machine& m, uint8_t* out, size_t cap, size_t& written, uint32_t version) {
    written = 0;
    if (!m.ok() || cap < STATE_HEADER_SIZE || version < 2 || version > STATE_VERSION) return false;
    if (version < 17 && m.ts6bga()) return false;
    if (version < 18 && m.ts6bga() && m.ts6bga()->bltBusy()) return false;
    if (version < STATE_FPU_BOARD2 && m.fpuBoard(1)) return false; // 版 15 までの並びには 2 枚目のボードがない
    StateIO s(StateIO::SAVE, out + STATE_HEADER_SIZE, cap - STATE_HEADER_SIZE, version);
    m.state(s);
    if (!s.ok()) return false;
    const size_t payload = s.pos();
    memset(out, 0, STATE_HEADER_SIZE);
    memcpy(out, kMagic, sizeof kMagic);
    put32(out + 8, version);
    put32(out + 12, (uint32_t)STATE_HEADER_SIZE);
    uint32_t fp[kFingerprintWords];
    fingerprint(m, fp);
    for (int i = 0; i < kFingerprintWords; i++) put32(out + 16 + 4 * i, fp[i]);
    put32(out + 48, (uint32_t)payload);
    put32(out + 52, (uint32_t)((uint64_t)payload >> 32));
    put32(out + 56, crc32(out + STATE_HEADER_SIZE, payload));
    put32(out + 60, boardBits(m));
    written = STATE_HEADER_SIZE + payload;
    return true;
}

bool loadState(Machine& m, const uint8_t* data, size_t size, const char*& error) {
    error = nullptr;
    if (!m.ok()) {
        error = "machine is not ready";
        return false;
    }
    if (size < STATE_HEADER_SIZE || memcmp(data, kMagic, sizeof kMagic) != 0) {
        error = "not a pix68k state file";
        return false;
    }
    // 版 2 は、拡張ボードがないときの版 3 と同じ並び (+60 は 0)。ボードのない Machine には読める (下で +60 を比べる)。
    // 版 3 は、版 4 から SPC の tempPending_ を除いた並び。版 4 は、版 5 から Machine の speedFrac_ を除いた並び。
    // 版 5 は、版 6 から 68000 のバスのウェイトと DRAM リフレッシュの端数を除いた並び (StateIO::version() を見て読み分ける)。
    // 版 6 は版 7 と同じ並びで、DMAC のアレイチェーンの BTC の数え方だけが違う (Dmac::state())。
    // 版 7 は、版 8 から CD-ROM 装置の状態を除いた並び。版 8 は、版 9 から 68060 の TLB と ATC を除いた並び
    const uint32_t version = get32(data + 8);
    if ((version < 2 || version > STATE_VERSION) || get32(data + 12) != STATE_HEADER_SIZE) {
        error = "unsupported state version";
        return false;
    }
    uint32_t fp[kFingerprintWords];
    fingerprint(m, fp);
    static const char* const kWhat[kFingerprintWords] = {
        "CPU model differs", "RAM size differs",  "high memory size differs", "local memory size differs",
        "CPU clock differs", "IPL ROM differs",   "built-in SCSI ROM differs", "SCSI board ROM differs"};
    for (int i = 0; i < kFingerprintWords; i++) {
        if (get32(data + 16 + 4 * i) != fp[i]) {
            error = kWhat[i];
            return false;
        }
    }
    // 版 15 までのファイルに 2 枚目のボード (bit4) はない (あれば作ったファイル。2 枚目をつないだ Machine とは
    // ボードの組み合わせが合わないので、どちらにしても読まない)
    const uint32_t boards = get32(data + 60);
    if ((version < 17 && (boards & 96u)) || boards != boardBits(m) || (version < STATE_FPU_BOARD2 && (boards & 16u))) {
        error = "expansion boards differ";
        return false;
    }
    const uint64_t payload = (uint64_t)get32(data + 48) | (uint64_t)get32(data + 52) << 32;
    if (payload != size - STATE_HEADER_SIZE) {
        error = "state file is truncated";
        return false;
    }
    const uint8_t* body = data + STATE_HEADER_SIZE;
    if (crc32(body, (size_t)payload) != get32(data + 56)) {
        error = "state file is broken (CRC mismatch)";
        return false;
    }
    // 並びを確かめてから戻す (CHECK は Machine を変えない)
    {
        StateIO check(StateIO::CHECK, (uint8_t*)body, (size_t)payload, version);
        m.state(check);
        if (!check.ok() || check.pos() != payload) {
            error = check.why() ? check.why() : "state file layout does not match";
            return false;
        }
    }
    StateIO s(StateIO::LOAD, (uint8_t*)body, (size_t)payload, version);
    m.state(s);
    if (!s.ok()) { // CHECK が通っていれば起きない
        error = "state file layout does not match";
        return false;
    }
    return true;
}

} // namespace x68k
