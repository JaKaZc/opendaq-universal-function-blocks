#include "jpeg.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>

namespace daq::modules::universal_renderer
{

namespace
{

constexpr std::array<uint8_t, 64> kZigzag = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                             41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// A canonical Huffman table, with a lookup of codes up to 9 bits long.
struct Huffman
{
    bool present = false;
    std::array<int, 18> maxCode{};
    std::array<int, 17> minCode{};
    std::array<int, 17> valuePointer{};
    std::array<uint8_t, 256> values{};
    // (length << 8) | value for a code of at most 9 bits, else 0.
    std::array<uint16_t, 512> fast{};
};

bool build(Huffman& table, const uint8_t* counts, const uint8_t* symbols, int total)
{
    if (total > 256)
        return false;
    table = Huffman{};
    std::memcpy(table.values.data(), symbols, static_cast<size_t>(total));
    int code = 0;
    int k = 0;
    for (int length = 1; length <= 16; length++)
    {
        table.valuePointer[length] = k;
        table.minCode[length] = code;
        const int n = counts[length - 1];
        for (int i = 0; i < n; i++)
        {
            if (length <= 9)
            {
                const int shift = 9 - length;
                for (int j = 0; j < (1 << shift); j++)
                    table.fast[(code << shift) | j] = static_cast<uint16_t>((length << 8) | symbols[k]);
            }
            code++;
            k++;
        }
        table.maxCode[length] = n ? code - 1 : -1;
        code <<= 1;
    }
    table.maxCode[17] = INT_MAX;
    table.present = true;
    return true;
}

/// Reads the entropy-coded data of a scan: bits most significant first,
/// with the stuffed zero after each 0xFF removed, stopping at a marker.
class BitReader
{
public:
    BitReader(const uint8_t* data, size_t size, size_t position)
        : data(data)
        , size(size)
        , position(position)
    {
    }

    int get(int count)
    {
        const int value = peek(count);
        buffer <<= count;
        filled -= count;
        return value;
    }

    int peek(int count)
    {
        fill();
        return static_cast<int>(buffer >> (32 - count));
    }

    void skip(int count)
    {
        buffer <<= count;
        filled -= count;
    }

    /// Moves past a restart marker and starts reading afresh.
    void restart()
    {
        buffer = 0;
        filled = 0;
        if (!atMarker)
            while (position + 1 < size && !(data[position] == 0xFF && data[position + 1] >= 0xD0 && data[position + 1] <= 0xD7))
                position++;
        position = std::min(size, position + 2);
        atMarker = false;
    }

    /// Where the marker that ended the data starts.
    size_t end() const { return position; }

private:
    const uint8_t* data;
    size_t size;
    size_t position;
    uint32_t buffer = 0;
    int filled = 0;
    bool atMarker = false;

    void fill()
    {
        while (filled <= 24)
        {
            uint32_t byte = 0;
            if (!atMarker && position < size)
            {
                byte = data[position];
                if (byte == 0xFF)
                {
                    const uint8_t next = position + 1 < size ? data[position + 1] : 0;
                    if (next == 0x00)
                    {
                        position += 2;
                    }
                    else
                    {
                        atMarker = true;
                        byte = 0;
                    }
                }
                else
                {
                    position++;
                }
            }
            buffer |= byte << (24 - filled);
            filled += 8;
        }
    }
};

int decodeSymbol(BitReader& reader, const Huffman& table)
{
    const uint16_t fast = table.fast[static_cast<size_t>(reader.peek(9))];
    if (fast)
    {
        reader.skip(fast >> 8);
        return fast & 0xFF;
    }
    int code = 0;
    for (int length = 1; length <= 16; length++)
    {
        code = (code << 1) | reader.get(1);
        if (table.maxCode[length] >= 0 && code <= table.maxCode[length])
            return table.values[static_cast<size_t>(table.valuePointer[length] + code - table.minCode[length])];
    }
    return -1;
}

int extend(BitReader& reader, int size)
{
    if (size == 0)
        return 0;
    int value = reader.get(size);
    if (value < (1 << (size - 1)))
        value += (-1 << size) + 1;
    return value;
}

struct Component
{
    int id = 0;
    int h = 1;
    int v = 1;
    int quant = 0;
    int dc = 0;
    int ac = 0;
    int prediction = 0;
    int stride = 0;
    int rows = 0;
    std::vector<uint8_t> plane;
};

const std::array<std::array<float, 8>, 8>& cosines()
{
    static const auto table = []
    {
        std::array<std::array<float, 8>, 8> out{};
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                out[x][u] = 0.5f * (u == 0 ? 1.0f / std::sqrt(2.0f) : 1.0f) * std::cos((2 * x + 1) * u * 3.14159265358979f / 16.0f);
        return out;
    }();
    return table;
}

/// The inverse DCT of one block, written into a component's plane.
void inverse(const std::array<float, 64>& coefficients, uint8_t* out, int stride)
{
    const auto& c = cosines();
    std::array<float, 64> rows{};
    for (int v = 0; v < 8; v++)
        for (int x = 0; x < 8; x++)
        {
            float sum = 0;
            for (int u = 0; u < 8; u++)
                sum += c[x][u] * coefficients[v * 8 + u];
            rows[v * 8 + x] = sum;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            float sum = 0;
            for (int v = 0; v < 8; v++)
                sum += c[y][v] * rows[v * 8 + x];
            out[y * stride + x] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(sum + 128.0f)), 0, 255));
        }
}

class Decoder
{
public:
    Decoder(const std::vector<uint8_t>& data)
        : data(data)
    {
    }

    bool run(int& width, int& height, std::vector<uint8_t>& rgb)
    {
        if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8)
            return false;
        size_t position = 2;
        bool scanned = false;
        while (position + 4 <= data.size())
        {
            if (data[position] != 0xFF)
            {
                position++;
                continue;
            }
            const uint8_t marker = data[position + 1];
            position += 2;
            if (marker == 0xFF)
            {
                position--;
                continue;
            }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
                continue;
            if (marker == 0xD9)
                break;
            const size_t length = static_cast<size_t>(data[position] << 8 | data[position + 1]);
            if (length < 2 || position + length > data.size())
                return false;
            const uint8_t* segment = &data[position + 2];
            const size_t segmentLength = length - 2;
            switch (marker)
            {
                case 0xDB:
                    if (!quantization(segment, segmentLength))
                        return false;
                    break;
                case 0xC4:
                    if (!huffman(segment, segmentLength))
                        return false;
                    break;
                case 0xC0:
                case 0xC1:
                    if (!frame(segment, segmentLength))
                        return false;
                    break;
                case 0xC2:
                case 0xC3:
                case 0xC5:
                case 0xC6:
                case 0xC7:
                case 0xC9:
                case 0xCA:
                case 0xCB:
                case 0xCD:
                case 0xCE:
                case 0xCF:
                    return false;  // progressive, lossless or arithmetic coding
                case 0xE1:
                    exifOrientation(segment, segmentLength);
                    break;
                case 0xDD:
                    if (segmentLength < 2)
                        return false;
                    restartInterval = segment[0] << 8 | segment[1];
                    break;
                case 0xDA:
                {
                    size_t end = 0;
                    if (!scan(segment, segmentLength, position + length, end))
                        return false;
                    scanned = true;
                    position = end;
                    continue;
                }
                default:
                    break;
            }
            position += length;
        }
        if (!scanned || components.empty())
            return false;
        convert(rgb);
        orient(rgb);
        width = orientation >= 5 ? imageHeight : imageWidth;
        height = orientation >= 5 ? imageWidth : imageHeight;
        return true;
    }

private:
    const std::vector<uint8_t>& data;
    std::array<std::array<uint16_t, 64>, 4> quant{};
    std::array<Huffman, 4> dcTables;
    std::array<Huffman, 4> acTables;
    std::vector<Component> components;
    int imageWidth = 0;
    int imageHeight = 0;
    int hMax = 1;
    int vMax = 1;
    int mcusX = 0;
    int mcusY = 0;
    int restartInterval = 0;
    // EXIF orientation: 1 as stored; cameras often store the sensor's
    // picture and say here how to turn it.
    int orientation = 1;

    void exifOrientation(const uint8_t* segment, size_t length)
    {
        if (length < 14 || std::memcmp(segment, "Exif\0\0", 6) != 0)
            return;
        const uint8_t* tiff = segment + 6;
        const size_t size = length - 6;
        const bool little = tiff[0] == 'I';
        const auto u16 = [&](size_t at) { return little ? tiff[at] | tiff[at + 1] << 8 : tiff[at] << 8 | tiff[at + 1]; };
        const auto u32 = [&](size_t at)
        { return little ? static_cast<uint32_t>(u16(at) | u16(at + 2) << 16) : static_cast<uint32_t>(u16(at) << 16 | u16(at + 2)); };
        const size_t directory = u32(4);
        if (directory + 2 > size)
            return;
        const int entries = u16(directory);
        for (int i = 0; i < entries; i++)
        {
            const size_t entry = directory + 2 + static_cast<size_t>(i) * 12;
            if (entry + 12 > size)
                return;
            if (u16(entry) == 0x0112)
            {
                const int value = u16(entry + 8);
                if (value >= 1 && value <= 8)
                    orientation = value;
                return;
            }
        }
    }

    /// Turns or mirrors the decoded picture as its EXIF orientation says.
    void orient(std::vector<uint8_t>& rgb) const
    {
        if (orientation == 1)
            return;
        const int w = imageWidth;
        const int h = imageHeight;
        const bool swapped = orientation >= 5;
        const int outWidth = swapped ? h : w;
        const int outHeight = swapped ? w : h;
        std::vector<uint8_t> out(rgb.size());
        for (int dy = 0; dy < outHeight; dy++)
            for (int dx = 0; dx < outWidth; dx++)
            {
                int sx = dx;
                int sy = dy;
                switch (orientation)
                {
                    case 2: sx = w - 1 - dx; break;
                    case 3: sx = w - 1 - dx; sy = h - 1 - dy; break;
                    case 4: sy = h - 1 - dy; break;
                    case 5: sx = dy; sy = dx; break;
                    case 6: sx = dy; sy = h - 1 - dx; break;
                    case 7: sx = w - 1 - dy; sy = h - 1 - dx; break;
                    case 8: sx = w - 1 - dy; sy = dx; break;
                    default: break;
                }
                std::memcpy(&out[(static_cast<size_t>(dy) * outWidth + dx) * 3], &rgb[(static_cast<size_t>(sy) * w + sx) * 3], 3);
            }
        rgb.swap(out);
    }

    bool quantization(const uint8_t* segment, size_t length)
    {
        size_t i = 0;
        while (i < length)
        {
            const int precision = segment[i] >> 4;
            const int id = segment[i] & 3;
            i++;
            for (int k = 0; k < 64; k++)
            {
                if (i + (precision ? 2 : 1) > length)
                    return false;
                quant[id][k] = precision ? static_cast<uint16_t>(segment[i] << 8 | segment[i + 1]) : segment[i];
                i += precision ? 2 : 1;
            }
        }
        return true;
    }

    bool huffman(const uint8_t* segment, size_t length)
    {
        size_t i = 0;
        while (i + 17 <= length)
        {
            const int kind = segment[i] >> 4;
            const int id = segment[i] & 3;
            const uint8_t* counts = &segment[i + 1];
            int total = 0;
            for (int k = 0; k < 16; k++)
                total += counts[k];
            if (i + 17 + total > length)
                return false;
            if (!build(kind ? acTables[id] : dcTables[id], counts, &segment[i + 17], total))
                return false;
            i += 17 + total;
        }
        return true;
    }

    bool frame(const uint8_t* segment, size_t length)
    {
        if (length < 6 || segment[0] != 8)
            return false;
        imageHeight = segment[1] << 8 | segment[2];
        imageWidth = segment[3] << 8 | segment[4];
        const int count = segment[5];
        if (imageWidth <= 0 || imageHeight <= 0 || imageWidth > 16384 || imageHeight > 16384 || (count != 1 && count != 3) ||
            length < 6 + 3 * static_cast<size_t>(count))
            return false;
        components.clear();
        for (int i = 0; i < count; i++)
        {
            Component component;
            component.id = segment[6 + i * 3];
            component.h = std::max(1, segment[7 + i * 3] >> 4);
            component.v = std::max(1, segment[7 + i * 3] & 15);
            component.quant = segment[8 + i * 3] & 3;
            if (component.h > 4 || component.v > 4)
                return false;
            hMax = std::max(hMax, component.h);
            vMax = std::max(vMax, component.v);
            components.push_back(component);
        }
        mcusX = (imageWidth + 8 * hMax - 1) / (8 * hMax);
        mcusY = (imageHeight + 8 * vMax - 1) / (8 * vMax);
        for (auto& component : components)
        {
            component.stride = mcusX * component.h * 8;
            component.rows = mcusY * component.v * 8;
            component.plane.assign(static_cast<size_t>(component.stride) * component.rows, 128);
        }
        return true;
    }

    bool block(BitReader& reader, Component& component, int column, int row)
    {
        const Huffman& dc = dcTables[component.dc];
        const Huffman& ac = acTables[component.ac];
        if (!dc.present || !ac.present)
            return false;
        const auto& q = quant[component.quant];
        std::array<float, 64> coefficients{};
        const int size = decodeSymbol(reader, dc);
        if (size < 0 || size > 16)
            return false;
        component.prediction += extend(reader, size);
        coefficients[0] = static_cast<float>(component.prediction * q[0]);
        for (int k = 1; k < 64;)
        {
            const int symbol = decodeSymbol(reader, ac);
            if (symbol < 0)
                return false;
            const int run = symbol >> 4;
            const int bits = symbol & 15;
            if (bits == 0)
            {
                if (run != 15)
                    break;
                k += 16;
                continue;
            }
            k += run;
            if (k > 63)
                break;
            coefficients[kZigzag[k]] = static_cast<float>(extend(reader, bits) * q[k]);
            k++;
        }
        if ((column + 1) * 8 > component.stride || (row + 1) * 8 > component.rows)
            return true;
        inverse(coefficients, &component.plane[static_cast<size_t>(row) * 8 * component.stride + static_cast<size_t>(column) * 8],
                component.stride);
        return true;
    }

    bool scan(const uint8_t* segment, size_t length, size_t dataStart, size_t& end)
    {
        if (length < 1)
            return false;
        const int count = segment[0];
        if (count < 1 || count > 4 || length < 1 + 2 * static_cast<size_t>(count) + 3)
            return false;
        std::vector<Component*> inScan;
        for (int i = 0; i < count; i++)
        {
            const int id = segment[1 + i * 2];
            const auto found = std::find_if(components.begin(), components.end(), [id](const Component& c) { return c.id == id; });
            if (found == components.end())
                return false;
            found->dc = segment[2 + i * 2] >> 4 & 3;
            found->ac = segment[2 + i * 2] & 3;
            found->prediction = 0;
            inScan.push_back(&*found);
        }

        BitReader reader(data.data(), data.size(), dataStart);
        int sinceRestart = 0;
        const auto restartIfDue = [&](bool more)
        {
            if (!restartInterval || !more)
                return;
            if (++sinceRestart == restartInterval)
            {
                reader.restart();
                for (auto* component : inScan)
                    component->prediction = 0;
                sinceRestart = 0;
            }
        };

        if (count == 1)
        {
            // A scan of one component goes block by block, over the
            // component's own size.
            Component& component = *inScan[0];
            const int columns = ((imageWidth * component.h + hMax - 1) / hMax + 7) / 8;
            const int rows = ((imageHeight * component.v + vMax - 1) / vMax + 7) / 8;
            for (int row = 0; row < rows; row++)
                for (int column = 0; column < columns; column++)
                {
                    if (!block(reader, component, column, row))
                        return false;
                    restartIfDue(row + 1 < rows || column + 1 < columns);
                }
        }
        else
        {
            for (int my = 0; my < mcusY; my++)
                for (int mx = 0; mx < mcusX; mx++)
                {
                    for (auto* component : inScan)
                        for (int by = 0; by < component->v; by++)
                            for (int bx = 0; bx < component->h; bx++)
                                if (!block(reader, *component, mx * component->h + bx, my * component->v + by))
                                    return false;
                    restartIfDue(my + 1 < mcusY || mx + 1 < mcusX);
                }
        }
        end = reader.end();
        return true;
    }

    void convert(std::vector<uint8_t>& rgb) const
    {
        rgb.resize(static_cast<size_t>(imageWidth) * imageHeight * 3);
        const auto sample = [&](const Component& component, int x, int y)
        {
            const int sx = x * component.h / hMax;
            const int sy = y * component.v / vMax;
            return static_cast<int>(component.plane[static_cast<size_t>(sy) * component.stride + sx]);
        };
        for (int y = 0; y < imageHeight; y++)
        {
            uint8_t* out = &rgb[static_cast<size_t>(y) * imageWidth * 3];
            for (int x = 0; x < imageWidth; x++, out += 3)
            {
                const int luma = sample(components[0], x, y);
                if (components.size() == 1)
                {
                    out[0] = out[1] = out[2] = static_cast<uint8_t>(luma);
                    continue;
                }
                const int cb = sample(components[1], x, y) - 128;
                const int cr = sample(components[2], x, y) - 128;
                // ITU-R BT.601 in 16.16 fixed point.
                const int r = luma + ((91881 * cr + 32768) >> 16);
                const int g = luma - ((22554 * cb + 46802 * cr + 32768) >> 16);
                const int b = luma + ((116130 * cb + 32768) >> 16);
                out[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
                out[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
                out[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            }
        }
    }
};

}  // namespace

bool decodeJpeg(const std::vector<uint8_t>& data, int& width, int& height, std::vector<uint8_t>& rgb)
{
    return Decoder(data).run(width, height, rgb);
}

}  // namespace daq::modules::universal_renderer
