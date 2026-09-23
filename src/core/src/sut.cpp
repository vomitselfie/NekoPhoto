// Clip Studio Paint brushes (.sut): an SQLite database. Each tool is a row of Node (its name and the Variant
// it uses), and each Variant row holds the settings in named columns. Column meanings are read from the
// column names and checked against a real file; the pressure and rotation "effectors" are coded blobs with
// no public description, and are left out rather than guessed.
//
// Tip images and paper textures are materials, one MaterialFile row each: a tar holding the material's
// layer file (data/material*.layer) and icedata/layerData.xml, whose tags say BrushPattern or PaperTexture.
// The layer file is Clip Studio's C2F container: an 8-byte signature, then chunks of [u32 LE length][type]
// [payload][u32 CRC]. The first dATA chunk (the layer database's first five pages) is enciphered; the
// largest holds the rest of that SQLite database as plain 1024-byte pages from page 6, which are walked here
// record by record, without SQLite. The image is either a PNG blob (its alpha is the density) or an
// Offscreen row: an Attribute blob giving the size and the grid, and BlockData blocks of 256-pixel tiles,
// each zlib-compressed, one 8-bit channel. A Variant names its materials by install path, which the
// embedded copies do not record, so the materials of each kind are matched to the brushes in the order the
// brushes first use them.
#include "brushformats.h"
#include "compositor/png.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string_view>
#include <zlib.h>

#ifdef COMPOSITOR_HAVE_SQLITE
#include <sqlite3.h>
#endif

namespace compositor {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

struct Span {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

/// The regular files of a tar archive, by name.
std::vector<std::pair<std::string, Span>> tarFiles(const uint8_t* data, size_t size) {
    std::vector<std::pair<std::string, Span>> out;
    for (size_t at = 0; size - at >= 512 && out.size() < 256;) {
        const uint8_t* header = data + at;
        if (header[0] == 0) break;   // the end: zero blocks
        const char* text = reinterpret_cast<const char*>(header);
        std::string name(text, strnlen(text, 100));
        if (std::memcmp(header + 257, "ustar", 5) == 0 && header[345]) name = std::string(text + 345, strnlen(text + 345, 155)) + "/" + name;
        uint64_t length = 0;
        for (int i = 124; i < 136 && header[i] && header[i] != ' '; i++) {
            if (header[i] < '0' || header[i] > '7') return out;
            length = length * 8 + uint64_t(header[i] - '0');
        }
        at += 512;
        if (length > size - at) break;
        if (header[156] == '0' || header[156] == 0) out.emplace_back(std::move(name), Span{data + at, size_t(length)});
        at += std::min<uint64_t>(size - at, (length + 511) / 512 * 512);
    }
    return out;
}

/// The plain pages of a C2F layer file's database: page `first` onwards, 1024 bytes each.
struct Pages {
    static constexpr size_t pageSize = 1024;
    static constexpr uint64_t first = 6;
    const uint8_t* data = nullptr;
    size_t count = 0;
    const uint8_t* page(uint64_t number) const { return number >= first && number - first < count ? data + (number - first) * pageSize : nullptr; }
};

std::optional<Pages> c2fPages(Span file) {
    static const uint8_t signature[8] = {0x89, 'C', '2', 'F', '\r', '\n', 0x1A, '\n'};
    if (file.size < 8 || std::memcmp(file.data, signature, 8) != 0) return std::nullopt;
    Pages best;
    for (size_t at = 8; file.size - at >= 12;) {
        const size_t length = le32(file.data + at);
        if (length > file.size - at - 12) break;
        const uint8_t* type = file.data + at + 4;
        // The page chunk: a two-byte prefix, then whole pages (the enciphered one is 5 pages and 10 bytes).
        if (std::memcmp(type, "dATA", 4) == 0 && length >= 2 + Pages::pageSize && (length - 2) % Pages::pageSize == 0
            && (length - 2) / Pages::pageSize > best.count) {
            best.data = type + 4 + 2;
            best.count = (length - 2) / Pages::pageSize;
        }
        if (std::memcmp(type, "TAIL", 4) == 0) break;
        at += 12 + length;
    }
    if (!best.count) return std::nullopt;
    return best;
}

bool varint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    value = 0;
    for (int i = 0; i < 9; i++) {
        if (p >= end) return false;
        const uint8_t byte = *p++;
        if (i == 8) { value = (value << 8) | byte; return true; }
        value = (value << 7) | (byte & 0x7F);
        if (byte < 0x80) return true;
    }
    return true;
}

/// A table leaf cell's whole payload: the part on its page and the chain of overflow pages after it.
bool cellPayload(const Pages& pages, const uint8_t* page, size_t pointer, std::vector<uint8_t>& out) {
    constexpr size_t usable = Pages::pageSize, maxLocal = usable - 35, minLocal = (usable - 12) * 32 / 255 - 23;
    if (pointer >= usable) return false;
    const uint8_t* p = page + pointer;
    const uint8_t* end = page + usable;
    uint64_t size = 0, rowid = 0;
    if (!varint(p, end, size) || !varint(p, end, rowid) || size > pages.count * usable) return false;
    size_t local = size_t(size);
    if (size > maxLocal) {
        const size_t k = minLocal + (size - minLocal) % (usable - 4);
        local = k > maxLocal ? minLocal : k;
    }
    const size_t room = size_t(end - p);
    if (local > room || (local < size && room - local < 4)) return false;
    out.assign(p, p + local);
    if (local == size) return true;
    for (uint32_t next = be32(p + local); out.size() < size;) {
        const uint8_t* overflow = pages.page(next);
        if (!overflow) return false;
        const size_t take = std::min<size_t>(size_t(size) - out.size(), usable - 4);
        out.insert(out.end(), overflow + 4, overflow + 4 + take);
        next = be32(overflow);
    }
    return true;
}

struct Field {
    enum class Kind { Null, Integer, Real, Text, Blob } kind = Kind::Null;
    int64_t integer = 0;
    Span bytes;
};

/// A record: the header of serial types, then the values.
bool parseRecord(const std::vector<uint8_t>& payload, std::vector<Field>& fields) {
    fields.clear();
    const uint8_t* p = payload.data();
    uint64_t headerSize = 0;
    if (!varint(p, payload.data() + payload.size(), headerSize) || headerSize > payload.size()) return false;
    const uint8_t* headerEnd = payload.data() + headerSize;
    size_t body = size_t(headerSize);
    while (p < headerEnd) {
        uint64_t type = 0;
        if (!varint(p, headerEnd, type) || fields.size() >= 4096) return false;
        Field field;
        uint64_t length = 0;
        if (type >= 1 && type <= 6) { static const uint8_t sizes[] = {0, 1, 2, 3, 4, 6, 8}; length = sizes[type]; field.kind = Field::Kind::Integer; }
        else if (type == 7) { length = 8; field.kind = Field::Kind::Real; }
        else if (type == 8 || type == 9) { field.kind = Field::Kind::Integer; field.integer = int64_t(type - 8); }
        else if (type >= 12) { length = (type - 12) / 2; field.kind = type % 2 ? Field::Kind::Text : Field::Kind::Blob; }
        else if (type != 0) return false;
        if (length > payload.size() - body) return false;
        const uint8_t* at = payload.data() + body;
        if (field.kind == Field::Kind::Integer && length) {
            uint64_t v = (at[0] & 0x80) ? ~uint64_t(0) : 0;
            for (size_t i = 0; i < length; i++) v = (v << 8) | at[i];
            field.integer = int64_t(v);
        }
        field.bytes = {at, size_t(length)};
        body += size_t(length);
        fields.push_back(field);
    }
    return true;
}

/// Every record on every table leaf page. The fields point into a buffer reused for the next record.
template <class Visit>
void forEachRecord(const Pages& pages, Visit&& visit) {
    std::vector<uint8_t> payload;
    std::vector<Field> fields;
    for (size_t i = 0; i < pages.count; i++) {
        const uint8_t* page = pages.data + i * Pages::pageSize;
        if (page[0] != 0x0D) continue;
        const size_t cells = std::min<size_t>(size_t(page[3]) << 8 | page[4], (Pages::pageSize - 8) / 2);
        for (size_t c = 0; c < cells; c++) {
            const size_t pointer = size_t(page[8 + 2 * c]) << 8 | page[9 + 2 * c];
            if (cellPayload(pages, page, pointer, payload) && parseRecord(payload, fields)) visit(fields);
        }
    }
}

/// UTF-16BE, as the layer database writes names inside its blobs.
template <size_t N>
std::array<uint8_t, 2 * (N - 1)> utf16be(const char (&text)[N]) {
    std::array<uint8_t, 2 * (N - 1)> out{};
    for (size_t i = 0; i + 1 < N; i++) out[2 * i + 1] = uint8_t(text[i]);
    return out;
}

/// An Offscreen row's image: the Attribute's "Parameter" entry (its name, then the width, height, columns
/// and rows of 256-pixel tiles) and the BlockData's tiles. Null when no tile has data.
std::shared_ptr<GrayImage> offscreenImage(Span attribute, Span blocks, int& tilesWithData) {
    static const auto parameter = utf16be("Parameter");
    static const auto beginChunk = utf16be("BlockDataBeginChunk");
    constexpr uint32_t tile = 256;
    tilesWithData = 0;
    const uint8_t* attributeEnd = attribute.data + attribute.size;
    const uint8_t* at = std::search(attribute.data, attributeEnd, parameter.begin(), parameter.end());
    if (at - attribute.data < 4 || be32(at - 4) != parameter.size() / 2 || size_t(attributeEnd - at) < parameter.size() + 16) return nullptr;
    at += parameter.size();
    const uint32_t width = be32(at), height = be32(at + 4), columns = be32(at + 8), rows = be32(at + 12);
    if (width == 0 || height == 0 || columns == 0 || rows == 0 || columns > 32 || rows > 32 || width > columns * tile || height > rows * tile) return nullptr;
    std::shared_ptr<GrayImage> canvas;
    std::vector<uint8_t> pixels(tile * tile);
    // Blocks: [u32 size][u32 name length][UTF-16BE name][body]; a begin chunk's body is the tile index, its
    // byte count, width and height, whether it has data, and then the compressed length (twice, big- and
    // little-endian) and the zlib stream.
    for (size_t pos = 0; blocks.size - pos >= 8;) {
        const uint8_t* block = blocks.data + pos;
        const size_t blockSize = be32(block);
        if (blockSize < 8 || blockSize > blocks.size - pos) break;
        const uint8_t* blockEnd = block + blockSize;
        const size_t nameBytes = size_t(be32(block + 4)) * 2;
        if (nameBytes == beginChunk.size() && nameBytes <= blockSize - 8 && std::equal(beginChunk.begin(), beginChunk.end(), block + 8)) {
            const uint8_t* body = block + 8 + nameBytes;
            if (blockEnd - body >= 28 && be32(body + 16) != 0) {
                const uint32_t index = be32(body), bytes = be32(body + 4), compressed = be32(body + 20);
                if (bytes == tile * tile && be32(body + 8) == tile && be32(body + 12) == tile && index < columns * rows
                    && compressed <= size_t(blockEnd - body - 28)) {
                    uLongf length = tile * tile;
                    if (uncompress(pixels.data(), &length, body + 28, compressed) == Z_OK && length == tile * tile) {
                        if (!canvas) canvas = std::make_shared<GrayImage>(int(columns * tile), int(rows * tile));
                        const int x0 = int(index % columns * tile), y0 = int(index / columns * tile);
                        for (uint32_t y = 0; y < tile; y++) std::memcpy(canvas->row(y0 + int(y)) + x0, pixels.data() + y * tile, tile);
                        tilesWithData++;
                    }
                }
            }
        }
        pos += blockSize;
    }
    if (!canvas) return nullptr;
    return cropGray(*canvas, 0, 0, int(width), int(height));
}

/// A PNG blob's alpha, when anything in it is not clear. Larger than 8192 pixels a side is refused unread.
std::shared_ptr<GrayImage> pngAlpha(Span blob) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (blob.size < 24 || std::memcmp(blob.data, signature, 8) != 0 || be32(blob.data + 16) > 8192 || be32(blob.data + 20) > 8192) return nullptr;
    auto image = decodePngImage(blob.data, blob.size);
    if (!image || image->width() <= 0 || image->height() <= 0) return nullptr;
    auto alpha = std::make_shared<GrayImage>(image->width(), image->height());
    uint8_t peak = 0;
    for (int y = 0; y < image->height(); y++)
        for (int x = 0; x < image->width(); x++) peak = std::max(peak, alpha->at(x, y) = image->pixel(x, y)[3]);
    return peak ? alpha : nullptr;
}

} // namespace

std::optional<ClipStudioMaterial> readClipStudioMaterial(const uint8_t* data, size_t size) {
    ClipStudioMaterial material;
    Span layer;
    for (const auto& [name, file] : tarFiles(data, size)) {
        const std::string_view path(name);
        auto endsWith = [&](std::string_view tail) { return path.size() >= tail.size() && path.substr(path.size() - tail.size()) == tail; };
        if (endsWith("icedata/layerData.xml"))
            material.texture = std::string_view(reinterpret_cast<const char*>(file.data), file.size).find("PaperTexture") != std::string_view::npos;
        else if (!layer.data && path.find("data/") != std::string_view::npos && endsWith(".layer"))
            layer = file;
    }
    auto pages = layer.data ? c2fPages(layer) : std::nullopt;
    if (!pages) return std::nullopt;
    // Tiles when there are any (the full-size layer), otherwise the largest PNG that is not clear.
    std::shared_ptr<GrayImage> tiles, png;
    int mostTiles = 0;
    forEachRecord(*pages, [&](const std::vector<Field>& fields) {
        for (const Field& field : fields)
            if (field.kind == Field::Kind::Blob)
                if (auto alpha = pngAlpha(field.bytes); alpha && (!png || size_t(alpha->width()) * alpha->height() > size_t(png->width()) * png->height())) png = alpha;
        if (fields.size() >= 6 && fields[4].kind == Field::Kind::Blob && fields[5].kind == Field::Kind::Blob) {
            int count = 0;
            if (auto image = offscreenImage(fields[4].bytes, fields[5].bytes, count); image && count >= mostTiles) { tiles = image; mostTiles = count; }
        }
    });
    material.image = tiles ? tiles : png;
    if (!material.image) return std::nullopt;
    // Clip Studio keeps soft tips faint (a soft round peaks near 95); the densest pixel is taken as full paint.
    GrayImage& image = *material.image;
    const uint8_t peak = *std::max_element(image.data(), image.data() + image.byteCount());
    if (peak == 0) return std::nullopt;
    for (size_t i = 0; i < image.byteCount(); i++) image.data()[i] = uint8_t((image.data()[i] * 255u + peak / 2) / peak);
    if (!material.texture) {
        const PixelBounds b = nonzeroBounds(image);
        material.image = cropGray(image, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    }
    return material;
}

#ifdef COMPOSITOR_HAVE_SQLITE

namespace {

/// A Variant's material reference (BrushPatternImageArray, TextureImage): a count, then each entry's length
/// and its install path in UTF-16LE, name and folder. The first entry's path stands for it.
std::string materialReference(const void* blob, int size) {
    const auto* p = static_cast<const uint8_t*>(blob);
    if (!p || size < 16 || be32(p + 4) == 0) return {};
    const size_t length = be32(p + 12);
    if (length == 0 || length > size_t(size) - 16) return std::string(reinterpret_cast<const char*>(p), size_t(size));
    return std::string(reinterpret_cast<const char*>(p + 16), length);
}

} // namespace

std::optional<BrushImport> readClipStudio(const std::string& path, const std::string& name, std::string* error) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (error) *error = db ? sqlite3_errmsg(db) : "cannot open the database";
        sqlite3_close(db);
        return std::nullopt;
    }
    BrushImport import;
    import.set = name;
    sqlite3_stmt* rows = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT n.NodeName, v.* FROM Node n JOIN Variant v ON v.VariantID = n.NodeVariantID ORDER BY n._PW_ID", -1, &rows, nullptr) != SQLITE_OK) {
        if (error) *error = std::string("not a Clip Studio brush: ") + sqlite3_errmsg(db);
        sqlite3_close(db);
        return std::nullopt;
    }
    // Columns by name, so that a file from a version with fewer of them still reads.
    std::map<std::string, int> column;
    for (int i = 1; i < sqlite3_column_count(rows); i++) column.emplace(sqlite3_column_name(rows, i), i);
    auto has = [&](const char* key) { auto it = column.find(key); return it != column.end() && sqlite3_column_type(rows, it->second) != SQLITE_NULL; };
    auto number = [&](const char* key, double fallback) { return has(key) ? sqlite3_column_double(rows, column[key]) : fallback; };
    auto reference = [&](const char* key) { return has(key) ? materialReference(sqlite3_column_blob(rows, column[key]), sqlite3_column_bytes(rows, column[key])) : std::string(); };

    struct Wants { std::string tip, texture; bool reverseTexture = false; };
    std::vector<Wants> wants;
    std::vector<std::string> tipOrder, textureOrder;   // references in the order the brushes first use them
    auto remember = [](std::vector<std::string>& order, const std::string& ref) { if (!ref.empty() && std::find(order.begin(), order.end(), ref) == order.end()) order.push_back(ref); };
    int watercolour = 0, dual = 0;
    while (sqlite3_step(rows) == SQLITE_ROW && import.brushes.size() < 1000) {
        TipPreset preset;
        if (const unsigned char* title = sqlite3_column_text(rows, 0)) preset.name = reinterpret_cast<const char*>(title);
        if (preset.name.empty()) preset.name = name + " " + std::to_string(import.brushes.size() + 1);
        // Sizes in pixels when their unit is 0; the other units are relative and taken as pixels too.
        const double size = std::clamp(number("BrushSize", 20), 1.0, 2000.0);
        preset.diameter = size;
        BrushTip& tip = preset.tip;
        tip.shape = roundTipImage(std::min(size, 256.0), std::clamp(number("BrushHardness", 100), 0.0, 100.0) / 100, 1);
        tip.spacing = std::clamp(number("BrushInterval", 10), 1.0, 1000.0) / 100;
        tip.roundness = std::clamp(number("BrushThickness", 100), 1.0, 100.0) / 100;
        tip.angle = number("BrushRotation", 0);
        tip.flow = std::clamp(number("BrushFlow", 100), 0.0, 100.0) / 100;
        if (number("BrushUseSpray", 0) != 0) {
            tip.scatter = std::clamp(number("BrushSpraySize", 0), 0.0, 20000.0) / size;
            tip.count = int(std::clamp(std::lround(number("BrushSprayDensity", 1)), 1L, 16L));
            tip.scatterBothAxes = true;   // a spray scatters all round the stroke
        }
        Wants want;
        if (number("BrushUsePatternImage", 0) != 0) want.tip = reference("BrushPatternImageArray");
        want.texture = reference("TextureImage");
        if (!want.texture.empty()) {
            tip.grainDepth = std::clamp(number("TextureDensity", 100), 0.0, 100.0) / 100;
            tip.grainScale = 100 / std::clamp(number("TextureScale2", number("TextureScale", 100)), 1.0, 10000.0);
            want.reverseTexture = number("TextureReverseDensity", 0) != 0;
        }
        remember(tipOrder, want.tip);
        remember(textureOrder, want.texture);
        watercolour += number("BrushUseWaterColor", 0) != 0;
        dual += number("UseDualBrush", 0) != 0;
        if (tip.normalize()) { import.brushes.push_back(std::move(preset)); wants.push_back(std::move(want)); }
    }
    sqlite3_finalize(rows);

    // The embedded materials, each kind in file order.
    std::vector<std::shared_ptr<GrayImage>> tips, textures;
    if (!tipOrder.empty() || !textureOrder.empty()) {
        sqlite3_stmt* files = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT FileData FROM MaterialFile ORDER BY _PW_ID", -1, &files, nullptr) == SQLITE_OK)
            for (size_t pixels = 0; pixels < 256'000'000 && sqlite3_step(files) == SQLITE_ROW;) {   // the budget every import has
                const auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(files, 0));
                if (auto material = blob ? readClipStudioMaterial(blob, size_t(sqlite3_column_bytes(files, 0))) : std::nullopt) {
                    pixels += material->image->byteCount();
                    (material->texture ? textures : tips).push_back(material->image);
                }
            }
        sqlite3_finalize(files);
    }
    sqlite3_close(db);

    int missingTips = 0, missingTextures = 0, textured = 0;
    for (size_t i = 0; i < import.brushes.size(); i++) {
        BrushTip& tip = import.brushes[i].tip;
        const Wants& want = wants[i];
        if (!want.tip.empty()) {
            const size_t k = size_t(std::find(tipOrder.begin(), tipOrder.end(), want.tip) - tipOrder.begin());
            if (k < tips.size()) tip.shape = tips[k];
            else missingTips++;
        }
        if (!want.texture.empty()) {
            const size_t k = size_t(std::find(textureOrder.begin(), textureOrder.end(), want.texture) - textureOrder.begin());
            if (k < textures.size()) {
                if (want.reverseTexture) {
                    auto inverted = std::make_shared<GrayImage>(*textures[k]);
                    for (size_t j = 0; j < inverted->byteCount(); j++) inverted->data()[j] = uint8_t(255 - inverted->data()[j]);
                    tip.grain = inverted;
                } else {
                    tip.grain = textures[k];
                }
                textured++;
            } else {
                missingTextures++;
            }
        }
    }
    auto note = [&](int count, const char* what) { if (count) import.notes.push_back(what + std::string(": ") + std::to_string(count)); };
    note(missingTips, "brushes whose tip image is not in the file (a round tip stands in)");
    note(missingTextures, "brushes whose paper texture is not in the file (they paint without it)");
    note(textured, "brushes with a paper texture, whose rotation, brightness and contrast are left out");
    note(watercolour, "brushes with watercolour or colour mixing, which is left out");
    note(dual, "brushes with a dual brush, which is left out");
    if (tipOrder.size() > 1 || textureOrder.size() > 1)
        import.notes.push_back("the file does not say which embedded image belongs to which brush; they are matched in the order the brushes use them");
    if (!import.brushes.empty()) import.notes.push_back("pressure and rotation settings are Clip Studio's own coding and are left out");
    if (import.brushes.empty()) { if (error) *error = "the file holds no Clip Studio brush this reader can use"; return std::nullopt; }
    return import;
}

#else

std::optional<BrushImport> readClipStudio(const std::string&, const std::string&, std::string* error) {
    if (error) *error = "this build reads no Clip Studio brushes (it was made without SQLite)";
    return std::nullopt;
}

#endif

} // namespace compositor
