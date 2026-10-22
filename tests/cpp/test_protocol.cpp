#include "test_main.hpp"

#include "wp/protocol.hpp"

#include <cstdio>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <filesystem>
#include <string>
#include <vector>

namespace {

// std::filesystem instead of getenv("TMPDIR"/"TEMP"/"TMP"): one standard call
// that resolves the platform's temp directory, no environment access to leak
// and nothing for MSVC to deprecate.
std::string tempPath(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("wireplay_test_") + tag + ".bin"))
        .string();
}

// The protocol under test genuinely speaks FILE*: the app reads a pipe and
// FrameReader::hasPending() polls its descriptor, so the API cannot take an
// ostream. Where a FILE* is unavoidable, use MSVC's bounds-checked fopen_s
// instead of suppressing the warning.
FILE* openBinary(const std::string& path, const char* mode) {
#ifdef _MSC_VER
    FILE* f = nullptr;
    return fopen_s(&f, path.c_str(), mode) == 0 ? f : nullptr;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

TEST(protocol_constants_match_the_engine) {
    // The engine is a separate process in another language. These numbers are
    // the contract between them, and a mismatch is silent corruption rather
    // than an error — so pin them here and in engine/tests/test_wire_protocol.py.
    CHECK_EQ(int(wp::kStatus), 0x01);
    CHECK_EQ(int(wp::kFatal), 0x02);
    CHECK_EQ(int(wp::kStats), 0x04);
    CHECK_EQ(int(wp::kDevice), 0x08);
    CHECK_EQ(int(wp::kVideoInit), 0x10);
    CHECK_EQ(int(wp::kVideoAU), 0x11);
    CHECK_EQ(int(wp::kAudioInit), 0x20);
    CHECK_EQ(int(wp::kAudioAU), 0x21);
    CHECK_EQ(int(wp::kAudioPcmInit), 0x22);
    CHECK_EQ(int(wp::kPing), 0xFE);
    CHECK_EQ(int(wp::kEnd), 0xFF);
    CHECK_EQ(int(wp::kPong), 0x81);
    CHECK_EQ(int(wp::kRequestKey), 0x82);
    CHECK_EQ(int(wp::kRotate), 0x83);
    CHECK_EQ(int(wp::kRestartStream), 0x84);
    CHECK_EQ(int(wp::kTouch), 0x85);
    CHECK_EQ(int(wp::kButton), 0x86);
    CHECK_EQ(int(wp::kSetBitrate), 0x87);
}

TEST(video_codec_ids_match_the_engine) {
    // The engine picks the decoder with these; see
    // engine/tests/test_wire_protocol.py for the Python half of the contract.
    CHECK_EQ(int(wp::kCodecHevc), 0);
    CHECK_EQ(int(wp::kCodecH264), 1);
}

TEST(frame_round_trip) {
    const std::string path = tempPath("roundtrip");
    FILE* w = openBinary(path, "wb");
    CHECK(w != nullptr);
    if (!w) return;

    const std::string json = R"({"state":"streaming","detail":"ok"})";
    const uint8_t binary[] = {0, 1, 2, 3, 0x10, 0x0A, 0x0D, 0xFF};
    CHECK(wp::writeFrame(w, wp::kStatus, json.data(), json.size()));
    CHECK(wp::writeFrame(w, wp::kVideoAU, binary, sizeof(binary)));
    CHECK(wp::writeFrame(w, wp::kEnd));
    std::fclose(w);

    FILE* r = openBinary(path, "rb");
    CHECK(r != nullptr);
    if (!r) return;
    wp::FrameReader reader(r);
    wp::Frame f;

    CHECK(reader.read(f));
    CHECK_EQ(int(f.type), int(wp::kStatus));
    CHECK(f.asString() == json);

    CHECK(reader.read(f));
    CHECK_EQ(int(f.type), int(wp::kVideoAU));
    CHECK_EQ(f.size, sizeof(binary));
    for (size_t i = 0; i < sizeof(binary); ++i) CHECK_EQ(int(f.payload[i]), int(binary[i]));

    CHECK(reader.read(f));
    CHECK_EQ(int(f.type), int(wp::kEnd));
    CHECK_EQ(f.size, size_t(0));

    CHECK(!reader.read(f));  // clean EOF
    CHECK_EQ(reader.frames_read, uint64_t(3));
    std::fclose(r);
    std::remove(path.c_str());
}

TEST(frame_rejects_bad_length) {
    const std::string path = tempPath("badlen");
    FILE* w = openBinary(path, "wb");
    if (!w) return;
    const uint8_t zero_len[] = {0, 0, 0, 0};                 // length 0 is illegal
    const uint8_t huge_len[] = {0x7F, 0xFF, 0xFF, 0xFF, 1};   // > kMaxFrame
    std::fwrite(zero_len, 1, sizeof(zero_len), w);
    std::fwrite(huge_len, 1, sizeof(huge_len), w);
    std::fclose(w);

    FILE* r = openBinary(path, "rb");
    if (!r) return;
    wp::FrameReader reader(r);
    wp::Frame f;
    CHECK(!reader.read(f));  // must refuse, not allocate 2 GB
    std::fclose(r);
    std::remove(path.c_str());
}

TEST(frame_truncated_stream_is_eof) {
    const std::string path = tempPath("truncated");
    FILE* w = openBinary(path, "wb");
    if (!w) return;
    const uint8_t partial[] = {0, 0, 0, 10, wp::kVideoAU, 1, 2};  // claims 10, gives 3
    std::fwrite(partial, 1, sizeof(partial), w);
    std::fclose(w);

    FILE* r = openBinary(path, "rb");
    if (!r) return;
    wp::FrameReader reader(r);
    wp::Frame f;
    CHECK(!reader.read(f));
    std::fclose(r);
    std::remove(path.c_str());
}

TEST(read_u32be) {
    const uint8_t data[] = {0x00, 0x01, 0x00, 0x00, 0x12, 0x90};
    const uint8_t* p = data;
    size_t rem = sizeof(data);
    uint32_t v = 0;
    CHECK(wp::readU32BE(p, rem, v));
    CHECK_EQ(v, uint32_t(0x00010000));
    CHECK_EQ(rem, size_t(2));
    CHECK(!wp::readU32BE(p, rem, v));  // only 2 bytes left
    CHECK_EQ(rem, size_t(2));          // and it must not consume them
}

TEST(has_pending_tracks_the_pipe) {
    // The window's event loop waits on this while the engine is silent:
    // empty pipe must say "nothing yet" (so the X stays responsive),
    // a written byte must say "pending", and a closed writer must say
    // "pending" too -- EOF is for read() to report, not a reason to
    // spin the wait loop forever. (Windows exercises the PeekNamedPipe
    // branch of the same contract.)
#ifdef _WIN32
    // CreatePipe + _open_osfhandle dance is covered by the app itself;
    // the POSIX branch below pins the semantics.
    return;
#else
    int fds[2];
    CHECK(::pipe(fds) == 0);
    FILE* r = ::fdopen(fds[0], "rb");
    CHECK(r != nullptr);
    if (!r) return;
    wp::FrameReader reader(r);
    CHECK(!reader.hasPending());
    const uint8_t b = 0x42;
    CHECK(::write(fds[1], &b, 1) == 1);
    CHECK(reader.hasPending());
    ::close(fds[1]);
    CHECK(reader.hasPending());  // EOF pending: read() will say false
    std::fclose(r);
#endif
}
