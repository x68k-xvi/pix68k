#include "media.h"
#include "test_check.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using pix68k_retro::Media;
void write(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary); f.write((const char*)data.data(), data.size());
}
void text(const fs::path& path, const std::string& value) { write(path, {value.begin(), value.end()}); }
std::vector<uint8_t> read(const fs::path& path) {
    std::vector<uint8_t> out;
    CHECK_EQ(pix68k_retro::readFile(path.string(), out, 256u << 20), true);
    return out;
}
int main() {
#ifdef PIX68K_VITA
    CHECK_EQ(pix68k_retro::absolutePath("ux0:/data/disks/../game.xdf") == "ux0:/data/game.xdf", true);
    CHECK_EQ(pix68k_retro::absolutePath("app0:/roms/game.xdf") == "app0:/roms/game.xdf", true);
    CHECK_EQ(pix68k_retro::absolutePath("ux0:data/game.xdf") == "ux0:/data/game.xdf", true);
    CHECK_EQ(pix68k_retro::absolutePath("ux0:/../game.xdf") == "ux0:/game.xdf", true);
    CHECK_EQ(pix68k_retro::absolutePath("app0:") == "app0:/", true);
#endif
    char pattern[] = "/tmp/pix68k-retro-media.XXXXXX";
    auto* temp = mkdtemp(pattern);
    if (!temp) return 1;
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{temp};
    fs::path dir(temp), a = dir / "disk one.xdf", b = dir / "disk two.XDF", list = dir / "pair.M3U";
    std::vector<uint8_t> original(1261568), second(1261568, 0x11);
    write(a, original); write(b, second);
    fs::permissions(a, fs::perms::owner_read);
    fs::permissions(b, fs::perms::owner_read);
    text(list, "\xef\xbb\xbf# two drives\r\n \tdisk one.xdf\r\n\n# comment\r\ndisk two.XDF\r\n");
    std::string error;
#ifdef PIX68K_VITA
    // POSIX上にVitaのマウント名を模したディレクトリを作り、ルートのM3Uも検証する。
    {
        struct RestoreDirectory { fs::path saved = fs::current_path(); ~RestoreDirectory() { fs::current_path(saved); } } restore;
        fs::create_directory(dir / "ux0:");
        write(dir / "ux0:" / "disk.xdf", original);
        text(dir / "ux0:list.m3u", "disk.xdf\n");
        fs::current_path(dir);
        Media relativeVita;
        CHECK_EQ(relativeVita.load("ux0:list.m3u", error), true);
        if (!relativeVita.floppies.empty()) CHECK_EQ(relativeVita.floppies[0]->path == "ux0:/disk.xdf", true);
        text("ux0:list.m3u", "ux0:disk.xdf\n");
        Media absoluteVita;
        CHECK_EQ(absoluteVita.load("ux0:list.m3u", error), true);
        if (!absoluteVita.floppies.empty()) CHECK_EQ(absoluteVita.floppies[0]->path == "ux0:/disk.xdf", true);
        text("ux0:list.m3u", "../disk.xdf\n");
        Media parentVita;
        CHECK_EQ(parentVita.load("ux0:list.m3u", error), true);
        if (!parentVita.floppies.empty()) CHECK_EQ(parentVita.floppies[0]->path == "ux0:/disk.xdf", true);
    }
#endif
    Media readonly;
    CHECK_EQ(readonly.load(list.string(), error), true);
    CHECK_EQ(readonly.floppies.size(), 2);
    CHECK_EQ(readonly.floppies[0]->floppy.writeProtected(), true);
    CHECK_EQ(readonly.floppies[1]->data == second, true);
    CHECK_EQ(readonly.floppies[0]->path == a.string(), true);

    Media writable;
    writable.writable = true; writable.saveDirectory = (dir / "saves").string();
    CHECK_EQ(writable.load(list.string(), error), true);
    CHECK_EQ(writable.floppies[0]->floppy.writeProtected(), false);
    auto& image = *writable.floppies[0];
    auto* sector = image.floppy.sector(0, 0);
    CHECK_EQ(sector != nullptr, true);
    if (!sector) return 1;
    auto initialIdentity = image.identity();
    CHECK_EQ(image.identity(), initialIdentity);
    image.floppy.data(sector)[0] = 0x57;
    image.floppy.markDirty(sector);
    CHECK_EQ(image.dirty(), true);
    auto changedIdentity = image.identity();
    CHECK_EQ(changedIdentity != initialIdentity, true);
    auto savePath = image.savePath;
    CHECK_EQ(fs::path(savePath).extension() == ".pxm", true); // EmulationStation のゲーム一覧に保存コピーを出さない
    CHECK_EQ(fs::path(savePath).parent_path() == dir / "saves" / "pix68k-media", true);
    CHECK_EQ(writable.flush(error), true);
    CHECK_EQ(image.dirty(), false);
    CHECK_EQ(image.identity(), changedIdentity);
    CHECK_EQ(read(a) == original, true); // 元イメージは変更しない
    CHECK_EQ(read(savePath)[sector->offset], 0x57);
    Media reloaded;
    reloaded.writable = true; reloaded.saveDirectory = writable.saveDirectory;
    CHECK_EQ(reloaded.load(a.string(), error), true);
    CHECK_EQ(reloaded.floppies[0]->data[sector->offset], 0x57);

    // 保存が失敗しても dirty を残す。再試行で回復できる。
    image.floppy.data(sector)[1] = 0x62; image.floppy.markDirty(sector);
    text(dir / "not-a-directory", "x");
    image.savePath = (dir / "not-a-directory" / "image.xdf").string();
    CHECK_EQ(image.flush(error), false);
    CHECK_EQ(image.dirty(), true);
    CHECK_EQ(image.flushIdle(error), false); // 再試行の待機中も失敗状態を保持する
    image.savePath = savePath;
    CHECK_EQ(image.flush(error), true);
    CHECK_EQ(image.flushIdle(error), true);
    CHECK_EQ(read(savePath)[sector->offset + 1], 0x62);
    CHECK_EQ(read(a) == original, true);

    // 保存コピーの破損は、新しいデータで上書きせずロード時に知らせる。
    write(savePath, {0});
    Media corrupt; corrupt.writable = true; corrupt.saveDirectory = writable.saveDirectory;
    CHECK_EQ(corrupt.load(a.string(), error), false);
    fs::remove(savePath);
    // 同じパスで元ディスクが入れ替わったときは、古いコピーを使わない。
    fs::permissions(a, fs::perms::owner_write, fs::perm_options::add);
    write(a, second);
    Media changed; changed.writable = true; changed.saveDirectory = writable.saveDirectory;
    CHECK_EQ(changed.load(a.string(), error), true);
    CHECK_EQ(changed.floppies[0]->savePath != savePath, true);
    CHECK_EQ(changed.floppies[0]->data == second, true);

    fs::path hdf = dir / "drive.HDF";
    std::vector<uint8_t> hard(256 * 64);
    write(hdf, hard);
    Media hd; hd.writable = true; hd.saveDirectory = writable.saveDirectory;
    CHECK_EQ(hd.load(hdf.string(), error), true);
    CHECK_EQ(hd.floppies.size(), 0);
    CHECK_EQ(hd.hard->hard.blocks(), 64);
    hd.hard->hard.block(1)[0] = 0x79;
    hd.hard->hard.markDirty(256);
    auto hardIdentity = hd.hard->identity();
    for (int i = 0; i < 240; i++) {
        hd.hard->hard.block(1)[1] = (uint8_t)i;
        hd.hard->hard.markDirty(257);
        CHECK_EQ(hd.flushIdle(error), true);
        CHECK_EQ(fs::exists(hd.hard->savePath), false);
    }
    CHECK_EQ(hd.hard->identity() != hardIdentity, true);
    for (int i = 0; i < 119; i++) CHECK_EQ(hd.flushIdle(error), true);
    CHECK_EQ(hd.hard->dirty(), false);
    CHECK_EQ(fs::exists(hd.hard->savePath), true);
    CHECK_EQ(hd.flush(error), true);
    Media hdReload; hdReload.writable = true; hdReload.saveDirectory = writable.saveDirectory;
    CHECK_EQ(hdReload.load(hdf.string(), error), true);
    CHECK_EQ(hdReload.hard->hard.block(1)[0], 0x79);
    CHECK_EQ(read(hdf) == hard, true);

    text(list, "# empty\n \r\n");
    Media empty; CHECK_EQ(empty.load(list.string(), error), false);
    text(list, "missing.xdf\n");
    Media missing; CHECK_EQ(missing.load(list.string(), error), false);
    std::string many;
    for (int i = 0; i < 65; i++) many += "disk one.xdf\n";
    text(list, many);
    Media tooMany; CHECK_EQ(tooMany.load(list.string(), error), false);
    text(list, "disk one.xdf\ndisk one.xdf\n");
    Media duplicate; duplicate.writable = true; duplicate.saveDirectory = writable.saveDirectory;
    CHECK_EQ(duplicate.load(list.string(), error), false);
    Media noSave; noSave.writable = true;
    CHECK_EQ(noSave.load(b.string(), error), false);
    write(hdf, {0});
    Media badHdf; CHECK_EQ(badHdf.load(hdf.string(), error), false);
#ifdef PIX68K_VITA
    fs::path oversizedHard = dir / "too-large-for-vita.hdf";
    { std::ofstream f(oversizedHard, std::ios::binary); f.seekp((65u << 20) - 1); f.put(0); }
    Media tooLargeForVita; CHECK_EQ(tooLargeForVita.load(oversizedHard.string(), error), false);
    std::string floppySet;
    for (int i = 0; i < 27; i++) floppySet += "disk two.XDF\n";
    text(list, floppySet);
    Media tooManyBytes; CHECK_EQ(tooManyBytes.load(list.string(), error), false);
#endif
    fs::path huge = dir / "huge.xdf";
    { std::ofstream f(huge, std::ios::binary); f.seekp(8u << 20); f.put(0); }
    Media tooBig; CHECK_EQ(tooBig.load(huge.string(), error), false);
    return finish();
}
