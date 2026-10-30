#include "png.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace daq::modules::universal_renderer
{

namespace
{

// ---------------------------------------------------------------- checksums

/// The CRC tables for eight bytes at a time: table[k][n] is the CRC of byte
/// n followed by k zero bytes.
const std::array<std::array<uint32_t, 256>, 8>& crcTables()
{
    static const auto tables = []
    {
        std::array<std::array<uint32_t, 256>, 8> out{};
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            out[0][n] = c;
        }
        for (uint32_t n = 0; n < 256; n++)
            for (int k = 1; k < 8; k++)
                out[k][n] = out[0][out[k - 1][n] & 0xFF] ^ (out[k - 1][n] >> 8);
        return out;
    }();
    return tables;
}

uint32_t crc32(const uint8_t* data, size_t length, uint32_t crc = 0)
{
    const auto& table = crcTables();
    crc ^= 0xFFFFFFFFu;
    size_t i = 0;
    for (; i + 8 <= length; i += 8)
    {
        const uint32_t low = crc ^ (data[i] | data[i + 1] << 8 | data[i + 2] << 16 | static_cast<uint32_t>(data[i + 3]) << 24);
        crc = table[7][low & 0xFF] ^ table[6][(low >> 8) & 0xFF] ^ table[5][(low >> 16) & 0xFF] ^ table[4][low >> 24] ^
              table[3][data[i + 4]] ^ table[2][data[i + 5]] ^ table[1][data[i + 6]] ^ table[0][data[i + 7]];
    }
    for (; i < length; i++)
        crc = table[0][(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t length)
{
    // Sixteen bytes at a time: the sums of a block, plain and weighted, are
    // independent of each other, so the compiler computes them in vectors.
    uint32_t a = 1;
    uint32_t b = 0;
    size_t i = 0;
    while (i < length)
    {
        // 5552 bytes is the most that cannot overflow before the modulo.
        const size_t end = std::min(length, i + 5552);
        for (; i + 16 <= end; i += 16)
        {
            uint32_t sum = 0;
            uint32_t weighted = 0;
            for (uint32_t k = 0; k < 16; k++)
            {
                sum += data[i + k];
                weighted += (16 - k) * data[i + k];
            }
            b += 16 * a + weighted;
            a += sum;
        }
        for (; i < end; i++)
        {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

/// The Adler-32 of two pieces joined, from the pieces' own, as zlib's
/// adler32_combine computes it.
uint32_t adler32Combine(uint32_t first, uint32_t second, size_t secondLength)
{
    constexpr uint32_t base = 65521;
    const uint32_t remainder = static_cast<uint32_t>(secondLength % base);
    uint32_t sum1 = first & 0xFFFF;
    uint32_t sum2 = static_cast<uint32_t>((static_cast<uint64_t>(remainder) * sum1) % base);
    sum1 += (second & 0xFFFF) + base - 1;
    sum2 += (first >> 16) + (second >> 16) + base - remainder;
    if (sum1 >= base)
        sum1 -= base;
    if (sum1 >= base)
        sum1 -= base;
    if (sum2 >= (base << 1))
        sum2 -= (base << 1);
    if (sum2 >= base)
        sum2 -= base;
    return sum1 | (sum2 << 16);
}

// ---------------------------------------------------------------- deflate

class BitWriter
{
public:
    /// Writes into `out`, which has room for everything that is written.
    explicit BitWriter(uint8_t* out)
        : start(out)
        , at(out)
    {
    }

    /// Writes `count` bits of `value`, least significant first.
    void bits(uint32_t value, int count)
    {
        buffer |= static_cast<uint64_t>(value) << filled;
        filled += count;
        if (filled >= 32)
        {
            for (int k = 0; k < 4; k++)
                at[k] = static_cast<uint8_t>(buffer >> (8 * k));
            at += 4;
            buffer >>= 32;
            filled -= 32;
        }
    }

    /// Writes the bits that are left, the last byte padded with zeros.
    void flush()
    {
        while (filled > 0)
        {
            *at++ = static_cast<uint8_t>(buffer);
            buffer >>= 8;
            filled -= 8;
        }
        buffer = 0;
        filled = 0;
    }

    size_t size() const { return static_cast<size_t>(at - start); }

private:
    uint8_t* start;
    uint8_t* at;
    uint64_t buffer = 0;
    int filled = 0;
};

struct Code
{
    uint16_t bits;
    uint8_t length;
};

// Deflate stores Huffman codes most significant bit first, the reverse of
// every other field, so the fixed codes are kept reversed.
uint16_t reversed(uint32_t value, int count)
{
    uint32_t out = 0;
    for (int i = 0; i < count; i++)
        out |= ((value >> i) & 1u) << (count - 1 - i);
    return static_cast<uint16_t>(out);
}

// Deflate's fixed Huffman code for each literal and length symbol.
const std::array<Code, 288>& symbolCodes()
{
    static const auto table = []
    {
        std::array<Code, 288> out{};
        for (int value = 0; value < 288; value++)
        {
            if (value < 144)
                out[value] = {reversed(0x30 + value, 8), 8};
            else if (value < 256)
                out[value] = {reversed(0x190 + value - 144, 9), 9};
            else if (value < 280)
                out[value] = {reversed(value - 256, 7), 7};
            else
                out[value] = {reversed(0xC0 + value - 280, 8), 8};
        }
        return out;
    }();
    return table;
}

void symbol(BitWriter& writer, int value)
{
    const Code code = symbolCodes()[value];
    writer.bits(code.bits, code.length);
}

constexpr std::array<int, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistanceBase = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// The length symbol of each match length, and the distance symbol of each
/// distance, looked up rather than searched for.
struct MatchCodes
{
    std::array<uint8_t, 259> length{};
    std::array<uint8_t, 512> distance{};

    MatchCodes()
    {
        for (int l = 0, value = 3; value <= 258; value++)
        {
            while (l < 28 && kLengthBase[l + 1] <= value)
                l++;
            length[value] = static_cast<uint8_t>(l);
        }
        // Distances to 256 by themselves, longer ones by their 128ths, as
        // zlib keeps them.
        for (int d = 0, value = 1; value <= 256; value++)
        {
            while (d < 29 && kDistanceBase[d + 1] <= value)
                d++;
            distance[value - 1] = static_cast<uint8_t>(d);
        }
        for (int d = 0, value = 257; value <= 32768; value += 128)
        {
            while (d < 29 && kDistanceBase[d + 1] <= value)
                d++;
            distance[256 + ((value - 1) >> 7)] = static_cast<uint8_t>(d);
        }
    }
};

void match(BitWriter& writer, int length, int distance)
{
    static const MatchCodes codes;
    const int l = codes.length[length];
    symbol(writer, 257 + l);
    writer.bits(length - kLengthBase[l], kLengthExtra[l]);
    const int d = distance <= 256 ? codes.distance[distance - 1] : codes.distance[256 + ((distance - 1) >> 7)];
    static const auto distanceCodes = []
    {
        std::array<uint16_t, 30> out{};
        for (int k = 0; k < 30; k++)
            out[k] = reversed(k, 5);
        return out;
    }();
    writer.bits(distanceCodes[d], 5);
    writer.bits(distance - kDistanceBase[d], kDistanceExtra[d]);
}

/// How many bytes from a and b on are equal, up to `limit`, compared eight at
/// a time.
int sameLength(const uint8_t* a, const uint8_t* b, int limit)
{
    int length = 0;
    while (length + 8 <= limit)
    {
        uint64_t x;
        uint64_t y;
        std::memcpy(&x, a + length, 8);
        std::memcpy(&y, b + length, 8);
        if (x != y)
            return length + __builtin_ctzll(x ^ y) / 8;
        length += 8;
    }
    while (length < limit && a[length] == b[length])
        length++;
    return length;
}

/// Runs `work(0)` to `work(count - 1)` at the same time, one on this thread.
template <typename Work>
void parallel(int count, const Work& work)
{
    std::vector<std::thread> threads;
    for (int k = 1; k < count; k++)
        threads.emplace_back([&work, k] { work(k); });
    work(0);
    for (auto& thread : threads)
        thread.join();
}

/// How many cores to encode a picture of `bytes` with.
int bandsFor(size_t bytes)
{
    if (bytes < 256 * 1024)
        return 1;
    return static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
}

/// Deflate blocks for data[begin, end), with the fixed Huffman code. Repeats
/// are found with a hash of the next three bytes and a short chain of earlier
/// places in the same band; it reads no byte outside the band, so other bands
/// can be written meanwhile. Filtered plots are mostly runs of zeros, which
/// are taken as repeats of the byte before without a search. A band that is
/// not the last ends on a byte boundary, with an empty stored block, so the
/// bands, compressed at the same time, join into one stream.
std::vector<uint8_t> deflateBand(const uint8_t* bytes, int begin, int end, bool last)
{
    constexpr int kWindow = 32768;
    constexpr int kHashBits = 15;
    constexpr int kMaxChain = 8;
    constexpr int kMaxMatch = 258;
    // Places inside a longer repeat are not remembered, as in zlib's fast
    // levels: a plot's background repeats for whole rows.
    constexpr int kMaxInsert = 16;

    // Nine bits a byte at most, for literals above 143. The buffer is not
    // cleared first; only what is written is kept.
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[static_cast<size_t>(end - begin) * 9 / 8 + 64]);
    BitWriter writer(buffer.get());
    writer.bits(last ? 1 : 0, 1);
    writer.bits(1, 2);  // fixed Huffman code

    std::vector<int> head(1 << kHashBits, -1);
    std::vector<int> previous(kWindow, -1);
    const auto hashAt = [&](int i)
    { return ((bytes[i] << 10) ^ (bytes[i + 1] << 5) ^ bytes[i + 2]) & ((1 << kHashBits) - 1); };
    const auto insert = [&](int i)
    {
        if (i + 2 >= end)
            return;
        const int hash = hashAt(i);
        previous[i % kWindow] = head[hash];
        head[hash] = i;
    };

    int i = begin;
    while (i < end)
    {
        const int limit = std::min(kMaxMatch, end - i);
        int bestLength = 0;
        int bestDistance = 0;
        if (i > begin && limit >= 3)
        {
            bestLength = sameLength(bytes + i - 1, bytes + i, limit);
            bestDistance = 1;
        }
        if (bestLength < 32 && i + 2 < end)
        {
            int candidate = head[hashAt(i)];
            for (int chain = 0; candidate >= 0 && i - candidate <= kWindow && chain < kMaxChain; chain++)
            {
                if (bestLength < limit && bytes[candidate + bestLength] == bytes[i + bestLength])
                {
                    const int length = sameLength(bytes + candidate, bytes + i, limit);
                    if (length > bestLength)
                    {
                        bestLength = length;
                        bestDistance = i - candidate;
                        if (length == limit)
                            break;
                    }
                }
                const int next = previous[candidate % kWindow];
                if (next >= candidate)
                    break;
                candidate = next;
            }
        }
        if (bestLength >= 3)
        {
            match(writer, bestLength, bestDistance);
            if (bestLength <= kMaxInsert)
                for (int k = 0; k < bestLength; k++)
                    insert(i + k);
            else
                insert(i);
            i += bestLength;
        }
        else
        {
            symbol(writer, bytes[i]);
            insert(i);
            i++;
        }
    }
    symbol(writer, 256);
    if (!last)
    {
        writer.bits(0, 1);  // not the last block
        writer.bits(0, 2);  // stored
        writer.flush();
        for (const uint8_t value : {0x00, 0x00, 0xFF, 0xFF})
            writer.bits(value, 8);
    }
    writer.flush();
    return std::vector<uint8_t>(buffer.get(), buffer.get() + writer.size());
}

// ---------------------------------------------------------------- PNG

void put32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(&out[start], out.size() - start));
}

}  // namespace

std::vector<uint8_t> encodePng(const std::vector<uint8_t>& rgb, int width, int height)
{
    // Each row is filtered with whichever of None, Sub and Up leaves the
    // smallest bytes, the usual heuristic. Up turns repeated rows into zeros.
    // Bands of rows are filtered, compressed and checksummed on several cores
    // at once, and joined into one zlib stream.
    const size_t stride = static_cast<size_t>(width) * 3;
    // Not cleared first: every byte is written by the band it belongs to.
    const size_t size = (stride + 1) * height;
    std::unique_ptr<uint8_t[]> filtered(new uint8_t[size]);
    const int bands = bandsFor(size);
    std::vector<std::vector<uint8_t>> parts(bands);
    std::vector<uint32_t> checks(bands);
    std::vector<size_t> lengths(bands);
    parallel(bands,
             [&](int k)
             {
                 const int first = height * k / bands;
                 const int last = height * (k + 1) / bands;
                 for (int y = first; y < last; y++)
                 {
                     const uint8_t* row = &rgb[y * stride];
                     const uint8_t* above = y > 0 ? &rgb[(y - 1) * stride] : nullptr;
                     uint8_t* target = &filtered[y * (stride + 1)];
                     // A row like the one above is all zeros with Up.
                     if (above && std::memcmp(row, above, stride) == 0)
                     {
                         target[0] = 2;
                         std::memset(target + 1, 0, stride);
                         continue;
                     }
                     // The size of a byte taken as signed, min(v, 256 - v),
                     // which the compiler computes sixteen bytes at a time.
                     const auto size = [](uint8_t value) { return std::min<uint8_t>(value, static_cast<uint8_t>(-value)); };
                     uint32_t none = 0;
                     uint32_t sub = 0;
                     uint32_t up = 0;
                     for (size_t x = 0; x < std::min<size_t>(3, stride); x++)
                     {
                         none += size(row[x]);
                         sub += size(row[x]);
                     }
                     for (size_t x = 3; x < stride; x++)
                     {
                         none += size(row[x]);
                         sub += size(static_cast<uint8_t>(row[x] - row[x - 3]));
                     }
                     if (above)
                         for (size_t x = 0; x < stride; x++)
                             up += size(static_cast<uint8_t>(row[x] - above[x]));
                     else
                         up = none;
                     if (none <= sub && none <= up)
                     {
                         target[0] = 0;
                         std::memcpy(target + 1, row, stride);
                     }
                     else if (sub <= up)
                     {
                         target[0] = 1;
                         std::memcpy(target + 1, row, std::min<size_t>(3, stride));
                         for (size_t x = 3; x < stride; x++)
                             target[1 + x] = static_cast<uint8_t>(row[x] - row[x - 3]);
                     }
                     else
                     {
                         target[0] = 2;
                         for (size_t x = 0; x < stride; x++)
                             target[1 + x] = static_cast<uint8_t>(row[x] - (above ? above[x] : 0));
                     }
                 }
                 const int begin = first * static_cast<int>(stride + 1);
                 const int end = last * static_cast<int>(stride + 1);
                 parts[k] = deflateBand(filtered.get(), begin, end, k + 1 == bands);
                 checks[k] = adler32(&filtered[begin], static_cast<size_t>(end - begin));
                 lengths[k] = static_cast<size_t>(end - begin);
             });
    std::vector<uint8_t> stream = {0x78, 0x01};
    uint32_t check = 1;
    for (int k = 0; k < bands; k++)
    {
        stream.insert(stream.end(), parts[k].begin(), parts[k].end());
        check = adler32Combine(check, checks[k], lengths[k]);
    }
    for (int shift = 24; shift >= 0; shift -= 8)
        stream.push_back(static_cast<uint8_t>(check >> shift));

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    put32(header, static_cast<uint32_t>(width));
    put32(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB, deflate, adaptive filtering, no interlace
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", stream);
    chunk(out, "IEND", {});
    return out;
}

}  // namespace daq::modules::universal_renderer
