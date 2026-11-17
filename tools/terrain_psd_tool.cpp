// ============================================================================
// tools/terrain_psd_tool.cpp
//
// Ferramenta de ida e volta entre os Splatmaps do simulador ("Branquinho") e
// um arquivo PSD multicamadas editável no Affinity Photo (`terreno_editavel.psd`).
//
// Modos de uso:
//   terrain_psd_tool.exe --init
//       Gera `data/terrain/terreno_editavel.psd` (2048x2048) a partir de:
//         - data/satelite_2012_low.png (fallback: data/terrain/macro_albedo.png)
//         - data/terrain/valid_mask.png
//         - data/terrain/splatmap_0.png & data/terrain/splatmap_1.png
//
//   terrain_psd_tool.exe --apply
//       Lê `data/terrain/terreno_editavel.psd` editado no Affinity Photo,
//       compõe os materiais respeitando a hierarquia das camadas e atualiza
//       `splatmap_0.png` e `splatmap_1.png`.
//
// Compilação standalone (MSYS2 UCRT64, sem DLL externa):
//   g++ -O3 -static tools/terrain_psd_tool.cpp -Icgvis-base-opengl/include -lz -o tools/terrain_psd_tool.exe
// ============================================================================

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <sys/stat.h>
#endif

namespace
{
// ----------------------------------------------------------------------------
// Constantes dos 8 materiais (mesma ordem de shader_fragment.glsl)
// ----------------------------------------------------------------------------
enum MaterialId
{
    MAT_ASPHALT_MAIN = 0, // splat0.r
    MAT_ASPHALT_SEC  = 1, // splat0.g
    MAT_SIDEWALK     = 2, // splat0.b
    MAT_GRASS        = 3, // splat0.a
    MAT_FOREST       = 4, // splat1.r
    MAT_DIRT         = 5, // splat1.g
    MAT_WATER        = 6, // splat1.b
    MAT_BUILDING     = 7  // splat1.a
};

struct MaterialLayerSpec
{
    int         mat_id;
    const char* name;
    uint8_t     r, g, b;
};

// Ordem no PSD: de BAIXO para CIMA (layer 0 = fundo, layer N-1 = topo)
// Camadas superiores cobrem visualmente as inferiores no Affinity Photo e
// têm prioridade automática ao recompor os pesos no `--apply`.
const MaterialLayerSpec kMaterialLayers[] = {
    { MAT_GRASS,        "3_Grama_Base",         124, 179,  66 }, // Verde Claro  #7CB342
    { MAT_FOREST,       "4_Mata_Arvores",        27,  94,  32 }, // Verde Escuro #1B5E20
    { MAT_DIRT,         "5_Terra_Barro",        141, 110,  99 }, // Marrom       #8D6E63
    { MAT_WATER,        "6_Agua_Lagoa",          30, 136, 229 }, // Azul         #1E88E5
    { MAT_BUILDING,     "7_Predios",            142,  36, 170 }, // Roxo         #8E24AA
    { MAT_SIDEWALK,     "2_Calcadas",             0, 172, 193 }, // Ciano        #00ACC1
    { MAT_ASPHALT_SEC,  "1_Asfalto_Secundario", 251, 140,   0 }, // Laranja      #FB8C00
    { MAT_ASPHALT_MAIN, "0_Asfalto_Principal",  229,  57,  53 }  // Vermelho     #E53935
};
const int kNumMaterialLayers = 8;

const char* kLayerSatBottom = "SATELITE_BASE_FUNDO";
const char* kLayerSatTop    = "SATELITE_GUIA_TOPO";

// ----------------------------------------------------------------------------
// Utilitários de caminho (funciona da raiz do repo ou de bin/Debug)
// ----------------------------------------------------------------------------
bool FileExists(const std::string& p)
{
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::string DetectRepoPrefix()
{
    if (FileExists("data/terrain/splatmap_0.png"))
        return "";
    if (FileExists("../../../data/terrain/splatmap_0.png"))
        return "../../../";
    return "";
}

// ----------------------------------------------------------------------------
// Utilitários Big-Endian para leitura e escrita binária
// ----------------------------------------------------------------------------
void WriteU8(std::vector<uint8_t>& out, uint8_t v)
{
    out.push_back(v);
}

void WriteU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void WriteI16(std::vector<uint8_t>& out, int16_t v)
{
    WriteU16(out, static_cast<uint16_t>(v));
}

void WriteI32(std::vector<uint8_t>& out, int32_t v)
{
    WriteU32(out, static_cast<uint32_t>(v));
}

void WriteBytes(std::vector<uint8_t>& out, const void* ptr, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(ptr);
    out.insert(out.end(), b, b + n);
}

void PatchU32(std::vector<uint8_t>& out, size_t offset, uint32_t v)
{
    out[offset + 0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out[offset + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[offset + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[offset + 3] = static_cast<uint8_t>(v & 0xFF);
}

uint16_t ReadU16(const uint8_t* p)
{
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

int16_t ReadI16(const uint8_t* p)
{
    return static_cast<int16_t>(ReadU16(p));
}

uint32_t ReadU32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

int32_t ReadI32(const uint8_t* p)
{
    return static_cast<int32_t>(ReadU32(p));
}

// ----------------------------------------------------------------------------
// Reamostragem bilinear RGB para ajustar a imagem de satélite para 2048x2048
// ----------------------------------------------------------------------------
std::vector<uint8_t> ResizeRGBBilinear(const uint8_t* src, int sw, int sh, int dw, int dh)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
    for (int y = 0; y < dh; ++y)
    {
        float fy = (dh > 1) ? (static_cast<float>(y) / static_cast<float>(dh - 1)) * static_cast<float>(sh - 1) : 0.0f;
        int   y0 = std::max(0, std::min(sh - 1, static_cast<int>(std::floor(fy))));
        int   y1 = std::min(sh - 1, y0 + 1);
        float ty = fy - static_cast<float>(y0);

        for (int x = 0; x < dw; ++x)
        {
            float fx = (dw > 1) ? (static_cast<float>(x) / static_cast<float>(dw - 1)) * static_cast<float>(sw - 1) : 0.0f;
            int   x0 = std::max(0, std::min(sw - 1, static_cast<int>(std::floor(fx))));
            int   x1 = std::min(sw - 1, x0 + 1);
            float tx = fx - static_cast<float>(x0);

            const uint8_t* p00 = src + (static_cast<size_t>(y0) * sw + x0) * 3;
            const uint8_t* p10 = src + (static_cast<size_t>(y0) * sw + x1) * 3;
            const uint8_t* p01 = src + (static_cast<size_t>(y1) * sw + x0) * 3;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * sw + x1) * 3;

            uint8_t* out_p = dst.data() + (static_cast<size_t>(y) * dw + x) * 3;
            for (int c = 0; c < 3; ++c)
            {
                float v0 = p00[c] * (1.0f - tx) + p10[c] * tx;
                float v1 = p01[c] * (1.0f - tx) + p11[c] * tx;
                float v  = v0 * (1.0f - ty) + v1 * ty;
                out_p[c] = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(std::lround(v)))));
            }
        }
    }
    return dst;
}

// ----------------------------------------------------------------------------
// PackBits RLE (Compressão e Descompressão padrão Adobe PSD)
// ----------------------------------------------------------------------------
void EncodePackBitsRow(const uint8_t* row, int width, std::vector<uint8_t>& out)
{
    int i = 0;
    while (i < width)
    {
        // Procura repetição de pelo menos 3 bytes iguais (ou 2 no fim/início)
        int run = 1;
        while (i + run < width && run < 128 && row[i + run] == row[i])
            ++run;

        if (run >= 3)
        {
            out.push_back(static_cast<uint8_t>(257 - run)); // -(run - 1)
            out.push_back(row[i]);
            i += run;
        }
        else
        {
            int lit_start = i;
            int lit_len   = 0;
            while (i < width && lit_len < 128)
            {
                int next_run = 1;
                while (i + next_run < width && next_run < 3 && row[i + next_run] == row[i])
                    ++next_run;
                if (next_run >= 3 && lit_len > 0)
                    break;
                if (next_run >= 3 && lit_len == 0)
                    break;
                ++i;
                ++lit_len;
            }
            if (lit_len > 0)
            {
                out.push_back(static_cast<uint8_t>(lit_len - 1));
                out.insert(out.end(), row + lit_start, row + lit_start + lit_len);
            }
        }
    }
}

bool DecodePackBitsRow(const uint8_t* src, size_t src_len, uint8_t* dst, int row_width)
{
    size_t sp = 0;
    int    dp = 0;
    while (dp < row_width && sp < src_len)
    {
        int8_t n = static_cast<int8_t>(src[sp++]);
        if (n >= 0)
        {
            int count = static_cast<int>(n) + 1;
            if (sp + static_cast<size_t>(count) > src_len)
                return false;
            int copy_n = std::min(count, row_width - dp);
            std::memcpy(dst + dp, src + sp, static_cast<size_t>(copy_n));
            sp += static_cast<size_t>(count);
            dp += copy_n;
        }
        else if (n != -128)
        {
            int count = 1 - static_cast<int>(n);
            if (sp >= src_len)
                return false;
            uint8_t val    = src[sp++];
            int     fill_n = std::min(count, row_width - dp);
            std::memset(dst + dp, val, static_cast<size_t>(fill_n));
            dp += fill_n;
        }
    }
    return dp == row_width;
}

// ----------------------------------------------------------------------------
// Gravador PNG RGBA usando zlib (CRC32 + blocos padrão IHDR / IDAT / IEND)
// ----------------------------------------------------------------------------
uint32_t ComputeCRC32(const uint8_t* data, size_t len)
{
    return static_cast<uint32_t>(crc32(0L, data, static_cast<uInt>(len)));
}

void WritePNGChunk(FILE* f, const char* type4, const uint8_t* payload, size_t len)
{
    uint8_t hdr[4] = {
        static_cast<uint8_t>((len >> 24) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>(len & 0xFF)
    };
    std::fwrite(hdr, 1, 4, f);

    std::vector<uint8_t> chunk_buf(4 + len);
    std::memcpy(chunk_buf.data(), type4, 4);
    if (len > 0)
        std::memcpy(chunk_buf.data() + 4, payload, len);
    std::fwrite(chunk_buf.data(), 1, chunk_buf.size(), f);

    uint32_t c = ComputeCRC32(chunk_buf.data(), chunk_buf.size());
    uint8_t crc_be[4] = {
        static_cast<uint8_t>((c >> 24) & 0xFF),
        static_cast<uint8_t>((c >> 16) & 0xFF),
        static_cast<uint8_t>((c >> 8) & 0xFF),
        static_cast<uint8_t>(c & 0xFF)
    };
    std::fwrite(crc_be, 1, 4, f);
}

bool SavePNGRGBA(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        std::fprintf(stderr, "ERRO: Nao foi possivel abrir \"%s\" para escrita.\n", path.c_str());
        return false;
    }

    const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    std::fwrite(sig, 1, 8, f);

    // IHDR: RGBA 8-bit (color_type = 6)
    std::vector<uint8_t> ihdr;
    WriteU32(ihdr, static_cast<uint32_t>(width));
    WriteU32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(0); // interlace
    WritePNGChunk(f, "IHDR", ihdr.data(), ihdr.size());

    // Prepara scanlines com filtro PNG Sub (1) para ótima compressão das máscaras
    size_t row_bytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> raw_scanlines(static_cast<size_t>(height) * (1 + row_bytes));
    for (int y = 0; y < height; ++y)
    {
        uint8_t*       dst_row = raw_scanlines.data() + static_cast<size_t>(y) * (1 + row_bytes);
        const uint8_t* src_row = rgba.data() + static_cast<size_t>(y) * row_bytes;
        dst_row[0] = 1; // Sub filter
        for (size_t x = 0; x < row_bytes; ++x)
        {
            uint8_t left = (x >= 4) ? src_row[x - 4] : 0;
            dst_row[1 + x] = static_cast<uint8_t>(src_row[x] - left);
        }
    }

    uLongf comp_bound = compressBound(static_cast<uLong>(raw_scanlines.size()));
    std::vector<uint8_t> comp_buf(comp_bound);
    if (compress2(comp_buf.data(), &comp_bound, raw_scanlines.data(), static_cast<uLong>(raw_scanlines.size()), 6) != Z_OK)
    {
        std::fclose(f);
        return false;
    }

    WritePNGChunk(f, "IDAT", comp_buf.data(), static_cast<size_t>(comp_bound));
    WritePNGChunk(f, "IEND", nullptr, 0);
    std::fclose(f);
    return true;
}

// ----------------------------------------------------------------------------
// Estruturas PSD em memória
// ----------------------------------------------------------------------------
struct PsdLayer
{
    std::string          name;
    uint8_t              opacity = 255;
    bool                 visible = true;
    int                  width   = 0;
    int                  height  = 0;
    // Buffers RGBA completos no tamanho do documento (W x H)
    std::vector<uint8_t> r, g, b, a;
};

void BuildFlatCompositeRGB(
    int                          W,
    int                          H,
    const std::vector<PsdLayer>& layers,
    std::vector<uint8_t>&        out_r,
    std::vector<uint8_t>&        out_g,
    std::vector<uint8_t>&        out_b
)
{
    size_t n = static_cast<size_t>(W) * static_cast<size_t>(H);
    out_r.assign(n, 0);
    out_g.assign(n, 0);
    out_b.assign(n, 0);

    for (const PsdLayer& L : layers)
    {
        if (!L.visible || L.opacity == 0)
            continue;
        float g_op = static_cast<float>(L.opacity) / 255.0f;
        for (size_t i = 0; i < n; ++i)
        {
            float alpha = (static_cast<float>(L.a[i]) / 255.0f) * g_op;
            if (alpha <= 0.0f)
                continue;
            float inv = 1.0f - alpha;
            out_r[i] = static_cast<uint8_t>(std::min(255, static_cast<int>(L.r[i] * alpha + out_r[i] * inv + 0.5f)));
            out_g[i] = static_cast<uint8_t>(std::min(255, static_cast<int>(L.g[i] * alpha + out_g[i] * inv + 0.5f)));
            out_b[i] = static_cast<uint8_t>(std::min(255, static_cast<int>(L.b[i] * alpha + out_b[i] * inv + 0.5f)));
        }
    }
}

bool WritePSD(const std::string& path, int W, int H, const std::vector<PsdLayer>& layers)
{
    std::vector<uint8_t> buf;
    buf.reserve(16 * 1024 * 1024);

    // 1. Header (26 bytes)
    WriteBytes(buf, "8BPS", 4);
    WriteU16(buf, 1);                      // Version 1
    for (int i = 0; i < 6; ++i) WriteU8(buf, 0);
    WriteU16(buf, 3);                      // 3 channels (composite RGB)
    WriteU32(buf, static_cast<uint32_t>(H));
    WriteU32(buf, static_cast<uint32_t>(W));
    WriteU16(buf, 8);                      // 8 bits per channel
    WriteU16(buf, 3);                      // RGB Color Mode

    // 2. Color Mode Data (0 bytes)
    WriteU32(buf, 0);

    // 3. Image Resources (0 bytes)
    WriteU32(buf, 0);

    // Pre-compresses all 4 channels (-1, 0, 1, 2) for each layer using PackBits RLE
    struct CompressedChannel
    {
        int16_t              id;
        std::vector<uint8_t> data; // includes 2-byte compression=1 + row counts + RLE data
    };
    std::vector<std::vector<CompressedChannel>> all_ch(layers.size());

    for (size_t li = 0; li < layers.size(); ++li)
    {
        const PsdLayer&        L     = layers[li];
        const int16_t          ids[] = { -1, 0, 1, 2 };
        const std::vector<uint8_t>* ch_ptrs[] = { &L.a, &L.r, &L.g, &L.b };

        all_ch[li].resize(4);
        for (int ci = 0; ci < 4; ++ci)
        {
            CompressedChannel& cc = all_ch[li][ci];
            cc.id = ids[ci];

            std::vector<uint16_t> row_counts(static_cast<size_t>(H));
            std::vector<uint8_t>  rle_payload;
            rle_payload.reserve(static_cast<size_t>(W) * static_cast<size_t>(H) / 4);

            const std::vector<uint8_t>& plane = *ch_ptrs[ci];
            for (int y = 0; y < H; ++y)
            {
                size_t before = rle_payload.size();
                EncodePackBitsRow(plane.data() + static_cast<size_t>(y) * W, W, rle_payload);
                row_counts[static_cast<size_t>(y)] = static_cast<uint16_t>(rle_payload.size() - before);
            }

            WriteU16(cc.data, 1); // Compression = 1 (PackBits RLE)
            for (int y = 0; y < H; ++y)
                WriteU16(cc.data, row_counts[static_cast<size_t>(y)]);
            cc.data.insert(cc.data.end(), rle_payload.begin(), rle_payload.end());
        }
    }

    // 4. Layer and Mask Information Section
    size_t section_len_pos = buf.size();
    WriteU32(buf, 0); // placeholder for section_length

    size_t layer_info_len_pos = buf.size();
    WriteU32(buf, 0); // placeholder for layer_info_length
    size_t layer_info_start = buf.size();

    WriteI16(buf, static_cast<int16_t>(layers.size()));

    for (size_t li = 0; li < layers.size(); ++li)
    {
        const PsdLayer& L = layers[li];

        WriteI32(buf, 0); // top
        WriteI32(buf, 0); // left
        WriteI32(buf, H); // bottom
        WriteI32(buf, W); // right
        WriteU16(buf, 4); // num channels

        for (int ci = 0; ci < 4; ++ci)
        {
            WriteI16(buf, all_ch[li][ci].id);
            WriteU32(buf, static_cast<uint32_t>(all_ch[li][ci].data.size()));
        }

        WriteBytes(buf, "8BIM", 4);
        WriteBytes(buf, "norm", 4);
        WriteU8(buf, L.opacity);
        WriteU8(buf, 0); // clipping: base
        WriteU8(buf, L.visible ? 0 : 2); // flags: bit 1 = 1 means hidden/invisible
        WriteU8(buf, 0); // filler

        size_t extra_len_pos = buf.size();
        WriteU32(buf, 0); // placeholder extra_data_len
        size_t extra_start = buf.size();

        WriteU32(buf, 0); // layer mask data length = 0
        WriteU32(buf, 0); // blending ranges length = 0

        // Layer name: Pascal string padded to a multiple of 4 bytes
        size_t name_len = std::min<size_t>(255, L.name.size());
        WriteU8(buf, static_cast<uint8_t>(name_len));
        WriteBytes(buf, L.name.data(), name_len);
        size_t total_name_bytes = 1 + name_len;
        while (total_name_bytes % 4 != 0)
        {
            WriteU8(buf, 0);
            ++total_name_bytes;
        }

        uint32_t extra_len = static_cast<uint32_t>(buf.size() - extra_start);
        PatchU32(buf, extra_len_pos, extra_len);
    }

    // Channel Image Data
    for (size_t li = 0; li < layers.size(); ++li)
    {
        for (int ci = 0; ci < 4; ++ci)
        {
            const std::vector<uint8_t>& cdata = all_ch[li][ci].data;
            buf.insert(buf.end(), cdata.begin(), cdata.end());
        }
    }

    // Garante que layer_info_length seja par (regra da especificação PSD)
    if ((buf.size() - layer_info_start) % 2 != 0)
        WriteU8(buf, 0);

    uint32_t layer_info_len = static_cast<uint32_t>(buf.size() - layer_info_start);
    PatchU32(buf, layer_info_len_pos, layer_info_len);

    // Global Layer Mask Info (0 bytes)
    WriteU32(buf, 0);

    uint32_t section_len = static_cast<uint32_t>(buf.size() - (section_len_pos + 4));
    PatchU32(buf, section_len_pos, section_len);

    // 5. Image Data Section (composite RGB RLE)
    std::vector<uint8_t> comp_r, comp_g, comp_b;
    BuildFlatCompositeRGB(W, H, layers, comp_r, comp_g, comp_b);

    WriteU16(buf, 1); // Compression = 1 (PackBits RLE)
    const std::vector<uint8_t>* comp_planes[] = { &comp_r, &comp_g, &comp_b };

    std::vector<uint16_t> merged_counts(static_cast<size_t>(3 * H));
    std::vector<uint8_t>  merged_rle;
    merged_rle.reserve(static_cast<size_t>(W) * static_cast<size_t>(H) / 2);

    for (int c = 0; c < 3; ++c)
    {
        const std::vector<uint8_t>& pl = *comp_planes[c];
        for (int y = 0; y < H; ++y)
        {
            size_t before = merged_rle.size();
            EncodePackBitsRow(pl.data() + static_cast<size_t>(y) * W, W, merged_rle);
            merged_counts[static_cast<size_t>(c) * H + static_cast<size_t>(y)] =
                static_cast<uint16_t>(merged_rle.size() - before);
        }
    }

    for (size_t i = 0; i < merged_counts.size(); ++i)
        WriteU16(buf, merged_counts[i]);
    buf.insert(buf.end(), merged_rle.begin(), merged_rle.end());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        std::fprintf(stderr, "ERRO: Nao foi possivel gravar \"%s\".\n", path.c_str());
        return false;
    }
    size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return written == buf.size();
}

// ----------------------------------------------------------------------------
// Leitor PSD completo (suporta Raw=0, PackBits RLE=1, ZIP=2, ZIP+Pred=3,
// caixas delimitadoras recortadas e nomes Pascal ou bloco `luni` UTF-16BE)
// ----------------------------------------------------------------------------
struct RawChannelRecord
{
    int16_t  id;
    uint32_t data_length;
};

struct RawLayerRecord
{
    int32_t                       top, left, bottom, right;
    std::vector<RawChannelRecord> channels;
    uint8_t                       opacity = 255;
    bool                          visible = true;
    std::string                   name;
    // Retângulo opcional de User Layer Mask (channel_id == -2)
    int32_t                       mask_top = 0, mask_left = 0, mask_bottom = 0, mask_right = 0;
    uint8_t                       mask_default_color = 255;
    bool                          has_user_mask = false;
};

bool DecodePSDChannelData(
    const uint8_t*        src,
    size_t                src_len,
    int                   ch_w,
    int                   ch_h,
    std::vector<uint8_t>& out_plane
)
{
    if (ch_w <= 0 || ch_h <= 0)
    {
        out_plane.clear();
        return true;
    }
    if (src_len < 2)
        return false;

    uint16_t compression = ReadU16(src);
    const uint8_t* payload     = src + 2;
    size_t         payload_len = src_len - 2;

    size_t total_pixels = static_cast<size_t>(ch_w) * static_cast<size_t>(ch_h);
    out_plane.assign(total_pixels, 0);

    if (compression == 0)
    {
        // Raw uncompressed
        if (payload_len < total_pixels)
            return false;
        std::memcpy(out_plane.data(), payload, total_pixels);
        return true;
    }
    else if (compression == 1)
    {
        // PackBits RLE: ch_h uint16 byte counts followed by RLE scanlines
        size_t counts_bytes = static_cast<size_t>(ch_h) * 2;
        if (payload_len < counts_bytes)
            return false;
        size_t data_pos = counts_bytes;
        for (int y = 0; y < ch_h; ++y)
        {
            uint16_t row_bytes = ReadU16(payload + static_cast<size_t>(y) * 2);
            if (data_pos + row_bytes > payload_len)
                return false;
            if (!DecodePackBitsRow(payload + data_pos, row_bytes, out_plane.data() + static_cast<size_t>(y) * ch_w, ch_w))
                return false;
            data_pos += row_bytes;
        }
        return true;
    }
    else if (compression == 2 || compression == 3)
    {
        // ZIP (zlib deflate) without or with horizontal differencing prediction
        uLongf dest_len = static_cast<uLongf>(total_pixels);
        if (uncompress(out_plane.data(), &dest_len, payload, static_cast<uLong>(payload_len)) != Z_OK)
            return false;

        if (compression == 3)
        {
            for (int y = 0; y < ch_h; ++y)
            {
                uint8_t* row = out_plane.data() + static_cast<size_t>(y) * ch_w;
                for (int x = 1; x < ch_w; ++x)
                    row[x] = static_cast<uint8_t>(row[x] + row[x - 1]);
            }
        }
        return true;
    }

    return false;
}

bool ReadPSD(
    const std::string&     path,
    int&                   out_W,
    int&                   out_H,
    std::vector<PsdLayer>& out_layers
)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
    {
        std::fprintf(stderr, "ERRO: Nao foi possivel abrir \"%s\".\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsize < 34)
    {
        std::fclose(f);
        return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(fsize));
    if (std::fread(data.data(), 1, data.size(), f) != data.size())
    {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    if (std::memcmp(data.data(), "8BPS", 4) != 0)
    {
        std::fprintf(stderr, "ERRO: \"%s\" nao e um arquivo PSD valido (assinatura ausente).\n", path.c_str());
        return false;
    }

    uint16_t version = ReadU16(data.data() + 4);
    if (version != 1)
    {
        std::fprintf(stderr, "ERRO: Apenas PSD v1 (nao PSB) e suportado (versao=%u).\n", version);
        return false;
    }

    out_H = static_cast<int>(ReadU32(data.data() + 14));
    out_W = static_cast<int>(ReadU32(data.data() + 18));
    uint16_t depth = ReadU16(data.data() + 22);
    uint16_t mode  = ReadU16(data.data() + 24);
    if (depth != 8 || mode != 3)
    {
        std::fprintf(stderr, "ERRO: O PSD deve estar em RGB 8-bit (depth=%u, mode=%u).\n", depth, mode);
        return false;
    }

    size_t pos = 26;
    // Skip Color Mode Data
    uint32_t color_mode_len = ReadU32(data.data() + pos);
    pos += 4 + color_mode_len;

    // Skip Image Resources
    if (pos + 4 > data.size()) return false;
    uint32_t resources_len = ReadU32(data.data() + pos);
    pos += 4 + resources_len;

    // Layer and Mask Info Section
    if (pos + 8 > data.size()) return false;
    uint32_t section_len = ReadU32(data.data() + pos);
    pos += 4;
    if (section_len == 0)
    {
        std::fprintf(stderr, "ERRO: PSD sem camadas.\n");
        return false;
    }

    uint32_t layer_info_len = ReadU32(data.data() + pos);
    pos += 4;
    if (layer_info_len < 2 || pos + layer_info_len > data.size())
        return false;

    int16_t raw_layer_count = ReadI16(data.data() + pos);
    pos += 2;
    int layer_count = (raw_layer_count < 0) ? -static_cast<int>(raw_layer_count) : static_cast<int>(raw_layer_count);

    std::vector<RawLayerRecord> records(static_cast<size_t>(layer_count));
    for (int li = 0; li < layer_count; ++li)
    {
        if (pos + 18 > data.size()) return false;
        RawLayerRecord& rec = records[static_cast<size_t>(li)];
        rec.top    = ReadI32(data.data() + pos + 0);
        rec.left   = ReadI32(data.data() + pos + 4);
        rec.bottom = ReadI32(data.data() + pos + 8);
        rec.right  = ReadI32(data.data() + pos + 12);

        uint16_t num_ch = ReadU16(data.data() + pos + 16);
        pos += 18;
        if (pos + static_cast<size_t>(num_ch) * 6 + 16 > data.size()) return false;

        rec.channels.resize(num_ch);
        for (uint16_t ci = 0; ci < num_ch; ++ci)
        {
            rec.channels[ci].id          = ReadI16(data.data() + pos);
            rec.channels[ci].data_length = ReadU32(data.data() + pos + 2);
            pos += 6;
        }

        // Skip blend mode sig (4) + key (4)
        pos += 8;
        rec.opacity   = data[pos + 0];
        uint8_t flags = data[pos + 2];
        rec.visible   = ((flags & 0x02) == 0);
        pos += 4; // opacity, clipping, flags, filler

        uint32_t extra_len = ReadU32(data.data() + pos);
        pos += 4;
        size_t extra_end = std::min(data.size(), pos + extra_len);

        // 1. Layer Mask Data
        if (pos + 4 <= extra_end)
        {
            uint32_t mask_len = ReadU32(data.data() + pos);
            pos += 4;
            if (mask_len >= 20 && pos + 20 <= extra_end)
            {
                rec.has_user_mask      = true;
                rec.mask_top           = ReadI32(data.data() + pos + 0);
                rec.mask_left          = ReadI32(data.data() + pos + 4);
                rec.mask_bottom        = ReadI32(data.data() + pos + 8);
                rec.mask_right         = ReadI32(data.data() + pos + 12);
                rec.mask_default_color = data[pos + 16];
            }
            pos = std::min(extra_end, pos + mask_len);
        }

        // 2. Blending Ranges
        if (pos + 4 <= extra_end)
        {
            uint32_t blend_len = ReadU32(data.data() + pos);
            pos = std::min(extra_end, pos + 4 + blend_len);
        }

        // 3. Layer Name Pascal String (padded to multiple of 4 bytes)
        if (pos < extra_end)
        {
            uint8_t plen = data[pos];
            size_t  avail = std::min<size_t>(plen, extra_end - (pos + 1));
            rec.name.assign(reinterpret_cast<const char*>(data.data() + pos + 1), avail);
            size_t padded_len = ((1 + static_cast<size_t>(plen) + 3) / 4) * 4;
            pos = std::min(extra_end, pos + padded_len);
        }

        // 4. Additional Layer Information blocks (busca `luni` Unicode se houver)
        while (pos + 12 <= extra_end)
        {
            if (std::memcmp(data.data() + pos, "8BIM", 4) != 0 &&
                std::memcmp(data.data() + pos, "8B64", 4) != 0)
                break;

            char     key[5] = { 0 };
            std::memcpy(key, data.data() + pos + 4, 4);
            uint32_t blk_len = ReadU32(data.data() + pos + 8);
            pos += 12;
            size_t blk_end = std::min(extra_end, pos + blk_len);

            if (std::strcmp(key, "luni") == 0 && pos + 4 <= blk_end)
            {
                uint32_t utf16_chars = ReadU32(data.data() + pos);
                std::string uname;
                for (uint32_t c = 0; c < utf16_chars && pos + 4 + c * 2 + 1 < blk_end; ++c)
                {
                    uint16_t ch16 = ReadU16(data.data() + pos + 4 + c * 2);
                    if (ch16 > 0 && ch16 < 128)
                        uname.push_back(static_cast<char>(ch16));
                    else if (ch16 >= 128)
                        uname.push_back('_');
                }
                if (!uname.empty())
                    rec.name = uname;
            }

            size_t padded_blk = ((static_cast<size_t>(blk_len) + 1) / 2) * 2;
            pos = std::min(extra_end, pos + padded_blk);
        }

        pos = extra_end;
    }

    // Channel Image Data
    out_layers.resize(static_cast<size_t>(layer_count));
    for (int li = 0; li < layer_count; ++li)
    {
        const RawLayerRecord& rec = records[static_cast<size_t>(li)];
        PsdLayer&             L   = out_layers[static_cast<size_t>(li)];

        L.name    = rec.name;
        L.opacity = rec.opacity;
        L.visible = rec.visible;
        L.width   = out_W;
        L.height  = out_H;

        size_t full_n = static_cast<size_t>(out_W) * static_cast<size_t>(out_H);
        L.r.assign(full_n, 0);
        L.g.assign(full_n, 0);
        L.b.assign(full_n, 0);
        // Se uma camada não possuir canal Alpha (-1), ela é considerada totalmente opaca (255)
        bool has_alpha_ch = false;
        for (const auto& c : rec.channels)
        {
            if (c.id == -1) { has_alpha_ch = true; break; }
        }
        L.a.assign(full_n, has_alpha_ch ? 0 : 255);

        int rec_w = std::max(0, static_cast<int>(rec.right - rec.left));
        int rec_h = std::max(0, static_cast<int>(rec.bottom - rec.top));

        for (size_t ci = 0; ci < rec.channels.size(); ++ci)
        {
            const RawChannelRecord& ch = rec.channels[ci];
            if (pos + ch.data_length > data.size())
                return false;

            const uint8_t* ch_ptr = data.data() + pos;
            pos += ch.data_length;

            if (ch.id >= -1 && ch.id <= 2)
            {
                std::vector<uint8_t> decoded;
                if (!DecodePSDChannelData(ch_ptr, ch.data_length, rec_w, rec_h, decoded))
                    return false;

                std::vector<uint8_t>* target =
                    (ch.id == -1) ? &L.a :
                    (ch.id == 0)  ? &L.r :
                    (ch.id == 1)  ? &L.g : &L.b;

                for (int ry = 0; ry < rec_h; ++ry)
                {
                    int gy = rec.top + ry;
                    if (gy < 0 || gy >= out_H) continue;
                    for (int rx = 0; rx < rec_w; ++rx)
                    {
                        int gx = rec.left + rx;
                        if (gx < 0 || gx >= out_W) continue;
                        (*target)[static_cast<size_t>(gy) * out_W + gx] =
                            decoded[static_cast<size_t>(ry) * rec_w + rx];
                    }
                }
            }
            else if (ch.id == -2 && rec.has_user_mask)
            {
                // Combina User Layer Mask caso o usuário tenha adicionado máscara vetorial/pixel no Affinity Photo
                int mw = std::max(0, static_cast<int>(rec.mask_right - rec.mask_left));
                int mh = std::max(0, static_cast<int>(rec.mask_bottom - rec.mask_top));
                std::vector<uint8_t> mdec;
                if (DecodePSDChannelData(ch_ptr, ch.data_length, mw, mh, mdec))
                {
                    for (int gy = 0; gy < out_H; ++gy)
                    {
                        for (int gx = 0; gx < out_W; ++gx)
                        {
                            uint8_t mval = rec.mask_default_color;
                            int     ry   = gy - rec.mask_top;
                            int     rx   = gx - rec.mask_left;
                            if (ry >= 0 && ry < mh && rx >= 0 && rx < mw)
                                mval = mdec[static_cast<size_t>(ry) * mw + rx];
                            size_t gi = static_cast<size_t>(gy) * out_W + gx;
                            L.a[gi] = static_cast<uint8_t>((static_cast<int>(L.a[gi]) * static_cast<int>(mval)) / 255);
                        }
                    }
                }
            }
        }
    }

    return true;
}

// ----------------------------------------------------------------------------
// Operação --init: Cria data/terrain/terreno_editavel.psd
// ----------------------------------------------------------------------------
int RunInit(const std::string& prefix)
{
    std::string terrain_dir = prefix + "data/terrain/";
    std::string splat0_path = terrain_dir + "splatmap_0.png";
    std::string splat1_path = terrain_dir + "splatmap_1.png";
    std::string sat_path    = prefix + "data/satelite_2012_low.png";
    std::string macro_path  = terrain_dir + "macro_albedo.png";
    std::string psd_path    = terrain_dir + "terreno_editavel.psd";

    stbi_set_flip_vertically_on_load(false);

    int sw0 = 0, sh0 = 0, sc0 = 0;
    uint8_t* splat0 = stbi_load(splat0_path.c_str(), &sw0, &sh0, &sc0, 4);
    if (!splat0)
    {
        std::fprintf(stderr, "ERRO: Nao foi possivel carregar \"%s\".\n", splat0_path.c_str());
        return 1;
    }

    int sw1 = 0, sh1 = 0, sc1 = 0;
    uint8_t* splat1 = stbi_load(splat1_path.c_str(), &sw1, &sh1, &sc1, 4);
    if (!splat1 || sw1 != sw0 || sh1 != sh0)
    {
        std::fprintf(stderr, "ERRO: Nao foi possivel carregar \"%s\" ou dimensoes incompativeis.\n", splat1_path.c_str());
        if (splat0) stbi_image_free(splat0);
        if (splat1) stbi_image_free(splat1);
        return 1;
    }

    const int W = sw0;
    const int H = sh0;
    size_t    N = static_cast<size_t>(W) * static_cast<size_t>(H);

    // Carrega satélite para as camadas de guia (base + topo semi-transparente)
    int      sat_w = 0, sat_h = 0, sat_c = 0;
    uint8_t* sat_raw = stbi_load(sat_path.c_str(), &sat_w, &sat_h, &sat_c, 3);
    if (!sat_raw)
        sat_raw = stbi_load(macro_path.c_str(), &sat_w, &sat_h, &sat_c, 3);

    std::vector<uint8_t> sat_rgb(N * 3, 64);
    if (sat_raw)
    {
        sat_rgb = ResizeRGBBilinear(sat_raw, sat_w, sat_h, W, H);
        stbi_image_free(sat_raw);
    }

    // Prepara as 10 camadas (de baixo para cima)
    std::vector<PsdLayer> layers(10);

    // Camada 0 (Fundo): SATELITE_BASE_FUNDO
    {
        PsdLayer& bg = layers[0];
        bg.name    = kLayerSatBottom;
        bg.opacity = 255;
        bg.visible = true;
        bg.width   = W;
        bg.height  = H;
        bg.r.resize(N);
        bg.g.resize(N);
        bg.b.resize(N);
        bg.a.assign(N, 255);
        for (size_t i = 0; i < N; ++i)
        {
            bg.r[i] = sat_rgb[i * 3 + 0];
            bg.g[i] = sat_rgb[i * 3 + 1];
            bg.b[i] = sat_rgb[i * 3 + 2];
        }
    }

    // Inicializa as 8 camadas de materiais
    for (int k = 0; k < kNumMaterialLayers; ++k)
    {
        PsdLayer& L = layers[static_cast<size_t>(1 + k)];
        L.name    = kMaterialLayers[k].name;
        L.opacity = 255;
        L.visible = true;
        L.width   = W;
        L.height  = H;
        L.r.assign(N, kMaterialLayers[k].r);
        L.g.assign(N, kMaterialLayers[k].g);
        L.b.assign(N, kMaterialLayers[k].b);
        L.a.assign(N, 0);
    }

    // Decompõe os pesos dos splatmaps de cima para baixo via inversão de Porter-Duff `over`
    // garantindo ida e volta fiel e mantendo 3_Grama_Base sólida na área válida
    for (size_t i = 0; i < N; ++i)
    {
        float w_mat[8];
        w_mat[MAT_ASPHALT_MAIN] = static_cast<float>(splat0[i * 4 + 0]);
        w_mat[MAT_ASPHALT_SEC]  = static_cast<float>(splat0[i * 4 + 1]);
        w_mat[MAT_SIDEWALK]     = static_cast<float>(splat0[i * 4 + 2]);
        w_mat[MAT_GRASS]        = static_cast<float>(splat0[i * 4 + 3]);
        w_mat[MAT_FOREST]       = static_cast<float>(splat1[i * 4 + 0]);
        w_mat[MAT_DIRT]         = static_cast<float>(splat1[i * 4 + 1]);
        w_mat[MAT_WATER]        = static_cast<float>(splat1[i * 4 + 2]);
        w_mat[MAT_BUILDING]     = static_cast<float>(splat1[i * 4 + 3]);

        float w_sum = 0.0f;
        for (int m = 0; m < 8; ++m) w_sum += w_mat[m];

        if (w_sum < 4.0f)
            continue;

        for (int m = 0; m < 8; ++m) w_mat[m] /= w_sum;

        // 3_Grama_Base (k = 0) preenche toda a área válida com Alpha = 255
        // para que apagar asfalto/mata em cima revele grama automaticamente
        layers[1].a[i] = 255;

        float rem = 1.0f;
        for (int k = kNumMaterialLayers - 1; k >= 1; --k)
        {
            float w = w_mat[kMaterialLayers[k].mat_id];
            float a = (rem > 1e-5f) ? std::max(0.0f, std::min(1.0f, w / rem)) : 0.0f;
            layers[static_cast<size_t>(1 + k)].a[i] = static_cast<uint8_t>(std::lround(a * 255.0f));
            rem -= w;
        }
    }

    // Camada 9 (Topo): SATELITE_GUIA_TOPO (45% opacidade sobre todas as máscaras)
    {
        PsdLayer& top = layers[9];
        top.name    = kLayerSatTop;
        top.opacity = 115; // ~45%
        top.visible = true;
        top.width   = W;
        top.height  = H;
        top.r       = layers[0].r;
        top.g       = layers[0].g;
        top.b       = layers[0].b;
        top.a.assign(N, 255);
    }

    stbi_image_free(splat0);
    stbi_image_free(splat1);

    std::printf("Gerando PSD editavel (%dx%d, 10 camadas) em \"%s\"...\n", W, H, psd_path.c_str());
    if (!WritePSD(psd_path, W, H, layers))
    {
        std::fprintf(stderr, "ERRO: Falha ao escrever \"%s\".\n", psd_path.c_str());
        return 1;
    }

    std::printf("OK! \"%s\" pronto para abrir no Affinity Photo.\n", psd_path.c_str());
    return 0;
}

// ----------------------------------------------------------------------------
// Operação --apply: Lê data/terrain/terreno_editavel.psd e atualiza os splatmaps
// ----------------------------------------------------------------------------
int RunApply(const std::string& prefix)
{
    std::string terrain_dir = prefix + "data/terrain/";
    std::string psd_path    = terrain_dir + "terreno_editavel.psd";
    std::string splat0_path = terrain_dir + "splatmap_0.png";
    std::string splat1_path = terrain_dir + "splatmap_1.png";

    int                   W = 0, H = 0;
    std::vector<PsdLayer> layers;
    if (!ReadPSD(psd_path, W, H, layers))
        return 1;

    std::printf("Lido \"%s\" (%dx%d, %zu camadas).\n", psd_path.c_str(), W, H, layers.size());
    size_t N = static_cast<size_t>(W) * static_cast<size_t>(H);

    // Mapeia cada material pela ordem hierárquica kMaterialLayers (case/prefix match por segurança)
    std::vector<const PsdLayer*> mat_layers(kNumMaterialLayers, nullptr);
    for (int k = 0; k < kNumMaterialLayers; ++k)
    {
        std::string target_name = kMaterialLayers[k].name;
        char        prefix_char = target_name[0];
        for (const PsdLayer& L : layers)
        {
            if (L.name == target_name)
            {
                mat_layers[k] = &L;
                break;
            }
            if (L.name.size() >= 2 && L.name[0] == prefix_char && L.name[1] == '_')
            {
                mat_layers[k] = &L;
                break;
            }
        }
        if (mat_layers[k])
            std::printf("  - Camada[%d]: %-22s (visivel=%s, opacidade=%u%%)\n",
                        k, mat_layers[k]->name.c_str(),
                        mat_layers[k]->visible ? "sim" : "nao",
                        static_cast<unsigned>((mat_layers[k]->opacity * 100) / 255));
        else
            std::printf("  - AVISO: Camada \"%s\" nao encontrada no PSD (peso zero).\n", target_name.c_str());
    }

    std::vector<uint8_t> splat0(N * 4, 0);
    std::vector<uint8_t> splat1(N * 4, 0);

    for (size_t i = 0; i < N; ++i)
    {
        // Compositing Porter-Duff `over` de cima para baixo:
        // camadas superiores têm prioridade sobre as inferiores sem apagar embaixo!
        float w_mat[8] = { 0 };
        float rem      = 1.0f;

        for (int k = kNumMaterialLayers - 1; k >= 0; --k)
        {
            const PsdLayer* L = mat_layers[k];
            if (!L || !L->visible || L->opacity == 0)
                continue;

            float a = (static_cast<float>(L->a[i]) / 255.0f) * (static_cast<float>(L->opacity) / 255.0f);
            a = std::max(0.0f, std::min(1.0f, a));

            float contrib = rem * a;
            w_mat[kMaterialLayers[k].mat_id] += contrib;
            rem *= (1.0f - a);
        }

        float total = 0.0f;
        for (int m = 0; m < 8; ++m) total += w_mat[m];

        // Se todas as camadas de material forem transparentes neste pixel, ele está fora do mapa (descartado pelo shader)
        if (total < 1e-4f)
            continue;

        for (int m = 0; m < 8; ++m) w_mat[m] /= total;

        // Quantiza os 8 pesos garantindo soma exata == 255 entre splat0 + splat1
        int q[8];
        int qsum = 0;
        for (int m = 0; m < 8; ++m)
        {
            q[m] = std::max(0, std::min(255, static_cast<int>(std::lround(w_mat[m] * 255.0f))));
            qsum += q[m];
        }
        if (qsum != 255)
        {
            int best_m = 0;
            for (int m = 1; m < 8; ++m)
            {
                if (q[m] > q[best_m]) best_m = m;
            }
            q[best_m] = std::max(0, std::min(255, q[best_m] + (255 - qsum)));
        }

        splat0[i * 4 + 0] = static_cast<uint8_t>(q[MAT_ASPHALT_MAIN]);
        splat0[i * 4 + 1] = static_cast<uint8_t>(q[MAT_ASPHALT_SEC]);
        splat0[i * 4 + 2] = static_cast<uint8_t>(q[MAT_SIDEWALK]);
        splat0[i * 4 + 3] = static_cast<uint8_t>(q[MAT_GRASS]);

        splat1[i * 4 + 0] = static_cast<uint8_t>(q[MAT_FOREST]);
        splat1[i * 4 + 1] = static_cast<uint8_t>(q[MAT_DIRT]);
        splat1[i * 4 + 2] = static_cast<uint8_t>(q[MAT_WATER]);
        splat1[i * 4 + 3] = static_cast<uint8_t>(q[MAT_BUILDING]);
    }

    if (!SavePNGRGBA(splat0_path, W, H, splat0) || !SavePNGRGBA(splat1_path, W, H, splat1))
        return 1;

    std::printf("Splatmaps atualizados: \"%s\" e \"%s\".\n", splat0_path.c_str(), splat1_path.c_str());
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    std::string mode = (argc >= 2) ? argv[1] : "--apply";
    std::string prefix = DetectRepoPrefix();

    if (mode == "--init")
        return RunInit(prefix);
    if (mode == "--apply")
        return RunApply(prefix);

    std::printf("Uso:\n  %s --init   (gera data/terrain/terreno_editavel.psd)\n  %s --apply  (aplica PSD editado nos splatmaps)\n",
                argv[0], argv[0]);
    return 1;
}
