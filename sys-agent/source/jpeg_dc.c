#include "jpeg_dc.h"

#include <string.h>

#define JPEG_MAX_COMPONENTS 4
#define JPEG_MAX_HUFFMAN 4

typedef struct {
    uint16_t table[64];          /* only index 0 (the DC step) is used */
} JpegQuant;

typedef struct {
    bool present;
    uint8_t symbols[256];
    int32_t minCode[17];
    int32_t maxCode[17];
    int32_t valPtr[17];
} JpegHuffman;

typedef struct {
    uint8_t id;
    uint8_t hFactor;
    uint8_t vFactor;
    uint8_t quantId;
    uint8_t dcTable;
    uint8_t acTable;
    int32_t dcPrediction;
    int blocksW;
    int blocksH;
} JpegComponent;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    const uint8_t* scan;
    size_t scanSize;
    size_t scanPos;
    uint32_t bitBuffer;
    int bitCount;
    JpegQuant quant[4];
    JpegHuffman huffman[JPEG_MAX_HUFFMAN * 2]; /* index = id*2 + (ac ? 1 : 0) */
    JpegComponent components[JPEG_MAX_COMPONENTS];
    int componentCount;
    int maxHFactor;
    int maxVFactor;
    int width;
    int height;
    int restartInterval;
} JpegDecoder;

/* --- Marker walking ------------------------------------------------------- */

static bool jpegReadU8(JpegDecoder* decoder, uint8_t* value)
{
    if (decoder->pos >= decoder->size)
        return false;
    *value = decoder->data[decoder->pos++];
    return true;
}

static bool jpegReadU16(JpegDecoder* decoder, uint16_t* value)
{
    if (decoder->pos + 1 >= decoder->size)
        return false;
    *value = (uint16_t)((decoder->data[decoder->pos] << 8) | decoder->data[decoder->pos + 1]);
    decoder->pos += 2;
    return true;
}

/* Returns the next marker, skipping the fill bytes a marker may be padded with. */
static int jpegNextMarker(JpegDecoder* decoder)
{
    while (decoder->pos + 1 < decoder->size) {
        if (decoder->data[decoder->pos] != 0xFF) {
            decoder->pos++;
            continue;
        }
        uint8_t marker = decoder->data[decoder->pos + 1];
        if (marker == 0xFF) {          /* fill byte */
            decoder->pos++;
            continue;
        }
        decoder->pos += 2;
        if (marker == 0x00)            /* not a marker after all */
            continue;
        return marker;
    }
    return -1;
}

static bool jpegParseQuant(JpegDecoder* decoder, uint16_t length)
{
    size_t end = decoder->pos + length;
    while (decoder->pos < end) {
        uint8_t spec = 0;
        if (!jpegReadU8(decoder, &spec))
            return false;
        int precision = spec >> 4;
        int id = spec & 0x0F;
        if (id > 3 || precision > 1)
            return false;
        for (int i = 0; i < 64; i++) {
            uint16_t value = 0;
            if (precision == 0) {
                uint8_t byte = 0;
                if (!jpegReadU8(decoder, &byte))
                    return false;
                value = byte;
            }
            else if (!jpegReadU16(decoder, &value)) {
                return false;
            }
            decoder->quant[id].table[i] = value;
        }
    }
    return decoder->pos == end;
}

static bool jpegParseHuffman(JpegDecoder* decoder, uint16_t length)
{
    size_t end = decoder->pos + length;
    while (decoder->pos < end) {
        uint8_t spec = 0;
        if (!jpegReadU8(decoder, &spec))
            return false;
        int isAc = spec >> 4;
        int id = spec & 0x0F;
        if (id > 3 || isAc > 1)
            return false;
        JpegHuffman* table = &decoder->huffman[id * 2 + isAc];
        uint8_t counts[17] = { 0 };
        uint32_t total = 0;
        for (int length = 1; length <= 16; length++) {
            if (!jpegReadU8(decoder, &counts[length]))
                return false;
            total += counts[length];
        }
        if (total > sizeof(table->symbols))
            return false;
        for (uint32_t i = 0; i < total; i++) {
            if (!jpegReadU8(decoder, &table->symbols[i]))
                return false;
        }

        /* Canonical code assignment (JPEG Annex C). */
        int32_t code = 0;
        uint32_t index = 0;
        for (int length = 1; length <= 16; length++) {
            if (counts[length]) {
                table->minCode[length] = code;
                table->valPtr[length] = (int32_t)index;
                code += counts[length];
                index += counts[length];
            }
            else {
                table->minCode[length] = -1;
                table->valPtr[length] = -1;
            }
            table->maxCode[length] = counts[length] ? code - 1 : -1;
            code <<= 1;
        }
        table->present = true;
    }
    return decoder->pos == end;
}

static bool jpegParseFrame(JpegDecoder* decoder, uint16_t length, uint8_t marker)
{
    if (marker != 0xC0)               /* baseline only */
        return false;
    uint8_t precision = 0;
    uint16_t height = 0, width = 0, componentCount = 0;
    if (!jpegReadU8(decoder, &precision) || precision != 8)
        return false;
    if (!jpegReadU16(decoder, &height) || !jpegReadU16(decoder, &width))
        return false;
    uint8_t declaredComponents = 0;
    if (!jpegReadU8(decoder, &declaredComponents))
        return false;
    componentCount = declaredComponents;
    if (componentCount == 0 || componentCount > JPEG_MAX_COMPONENTS)
        return false;
    if (length != (uint16_t)(8 + 3 * componentCount))
        return false;
    decoder->width = width;
    decoder->height = height;
    decoder->componentCount = componentCount;
    decoder->maxHFactor = 1;
    decoder->maxVFactor = 1;
    for (int i = 0; i < componentCount; i++) {
        uint8_t id = 0, sampling = 0, quantId = 0;
        if (!jpegReadU8(decoder, &id) || !jpegReadU8(decoder, &sampling) ||
            !jpegReadU8(decoder, &quantId))
            return false;
        JpegComponent* component = &decoder->components[i];
        component->id = id;
        component->hFactor = (uint8_t)(sampling >> 4);
        component->vFactor = (uint8_t)(sampling & 0x0F);
        component->quantId = quantId;
        if (component->hFactor == 0 || component->hFactor > 4 ||
            component->vFactor == 0 || component->vFactor > 4)
            return false;
        if (component->hFactor > decoder->maxHFactor)
            decoder->maxHFactor = component->hFactor;
        if (component->vFactor > decoder->maxVFactor)
            decoder->maxVFactor = component->vFactor;
    }
    for (int i = 0; i < componentCount; i++) {
        JpegComponent* component = &decoder->components[i];
        int sampledW = (decoder->width * component->hFactor + decoder->maxHFactor - 1)
                     / decoder->maxHFactor;
        int sampledH = (decoder->height * component->vFactor + decoder->maxVFactor - 1)
                     / decoder->maxVFactor;
        component->blocksW = (sampledW + 7) / 8;
        component->blocksH = (sampledH + 7) / 8;
    }
    return decoder->width > 0 && decoder->height > 0;
}

static bool jpegParseScan(JpegDecoder* decoder, uint16_t length)
{
    uint8_t count = 0;
    if (!jpegReadU8(decoder, &count) || count == 0 || count > decoder->componentCount)
        return false;
    if (length != (uint16_t)(6 + 2 * count))
        return false;
    for (int i = 0; i < count; i++) {
        uint8_t id = 0, tables = 0;
        if (!jpegReadU8(decoder, &id) || !jpegReadU8(decoder, &tables))
            return false;
        JpegComponent* component = NULL;
        for (int j = 0; j < decoder->componentCount; j++) {
            if (decoder->components[j].id == id) {
                component = &decoder->components[j];
                break;
            }
        }
        if (component == NULL)
            return false;
        component->dcTable = (uint8_t)(tables >> 4);
        component->acTable = (uint8_t)(tables & 0x0F);
    }
    /* Ss, Se and Ah/Al. Baseline uses one interleaved scan over all 64
     * coefficients, i.e. 0, 63, 0. */
    uint8_t spectralStart = 0, spectralEnd = 0, approximation = 0;
    if (!jpegReadU8(decoder, &spectralStart) || !jpegReadU8(decoder, &spectralEnd) ||
        !jpegReadU8(decoder, &approximation))
        return false;
    if (spectralStart != 0 || spectralEnd != 63 || approximation != 0)
        return false;
    return true;
}

/* --- Entropy decoding ---------------------------------------------------- */

static int jpegReadBit(JpegDecoder* decoder)
{
    if (decoder->bitCount == 0) {
        if (decoder->scanPos >= decoder->scanSize)
            return -1;
        uint8_t byte = decoder->scan[decoder->scanPos++];
        if (byte == 0xFF) {
            if (decoder->scanPos >= decoder->scanSize)
                return -1;
            uint8_t next = decoder->scan[decoder->scanPos];
            if (next == 0x00)
                decoder->scanPos++;        /* stuffed byte */
            else
                return -1;                 /* restart or end marker */
        }
        decoder->bitBuffer = byte;
        decoder->bitCount = 8;
    }
    decoder->bitCount--;
    return (int)((decoder->bitBuffer >> decoder->bitCount) & 1);
}

static int jpegReadBits(JpegDecoder* decoder, int count)
{
    int value = 0;
    for (int i = 0; i < count; i++) {
        int bit = jpegReadBit(decoder);
        if (bit < 0)
            return -1;
        value = (value << 1) | bit;
    }
    return value;
}

static int jpegDecodeHuffman(JpegDecoder* decoder, const JpegHuffman* table)
{
    if (!table->present)
        return -1;
    int32_t code = 0;
    for (int length = 1; length <= 16; length++) {
        int bit = jpegReadBit(decoder);
        if (bit < 0)
            return -1;
        code = (code << 1) | bit;
        if (table->maxCode[length] >= 0 && code <= table->maxCode[length]) {
            int32_t index = table->valPtr[length] + (code - table->minCode[length]);
            if (index < 0 || index >= (int32_t)sizeof(table->symbols))
                return -1;
            return table->symbols[index];
        }
    }
    return -1;
}

static int jpegExtend(int value, int bits)
{
    if (bits == 0)
        return 0;
    if (value < (1 << (bits - 1)))
        return value - (1 << bits) + 1;
    return value;
}

/* Walks the scan, keeping the DC coefficient of every luma block. Chroma DC and
 * all AC coefficients are consumed bit-exactly but not stored: the classifier
 * only needs brightness, and skipping keeps the decoder small. */
static bool jpegDecodeScan(JpegDecoder* decoder, JpegDcImage* image)
{
    int mcusW = (decoder->width + 8 * decoder->maxHFactor - 1) / (8 * decoder->maxHFactor);
    int mcusH = (decoder->height + 8 * decoder->maxVFactor - 1) / (8 * decoder->maxVFactor);
    int lumaBlocksW = decoder->components[0].blocksW;
    int lumaBlocksH = decoder->components[0].blocksH;
    if (lumaBlocksW > JPEG_DC_MAX_BLOCKS_X || lumaBlocksH > JPEG_DC_MAX_BLOCKS_Y)
        return false;

    memset(image->mean, 0, sizeof(image->mean));
    image->blocksW = lumaBlocksW;
    image->blocksH = lumaBlocksH;
    image->complete = false;

    int mcusSinceRestart = 0;
    for (int mcuY = 0; mcuY < mcusH; mcuY++) {
        for (int mcuX = 0; mcuX < mcusW; mcuX++) {
            if (decoder->restartInterval > 0 &&
                mcusSinceRestart == decoder->restartInterval) {
                /* The restart marker is byte-aligned and resets the predictors. */
                if (decoder->scanPos + 1 >= decoder->scanSize)
                    return false;
                if (decoder->scan[decoder->scanPos] != 0xFF ||
                    (decoder->scan[decoder->scanPos + 1] & 0xF8) != 0xD0)
                    return false;
                decoder->scanPos += 2;
                decoder->bitCount = 0;
                for (int i = 0; i < decoder->componentCount; i++)
                    decoder->components[i].dcPrediction = 0;
                mcusSinceRestart = 0;
            }

            for (int c = 0; c < decoder->componentCount; c++) {
                JpegComponent* component = &decoder->components[c];
                const JpegHuffman* dcTable = &decoder->huffman[component->dcTable * 2];
                const JpegHuffman* acTable = &decoder->huffman[component->acTable * 2 + 1];
                for (int blockY = 0; blockY < component->vFactor; blockY++) {
                    for (int blockX = 0; blockX < component->hFactor; blockX++) {
                        int symbol = jpegDecodeHuffman(decoder, dcTable);
                        if (symbol < 0)
                            return false;
                        int difference = 0;
                        if (symbol > 0) {
                            int bits = jpegReadBits(decoder, symbol);
                            if (bits < 0)
                                return false;
                            difference = jpegExtend(bits, symbol);
                        }
                        component->dcPrediction += difference;

                        if (c == 0) {
                            int blockColumn = mcuX * component->hFactor + blockX;
                            int blockRow = mcuY * component->vFactor + blockY;
                            if (blockColumn < lumaBlocksW && blockRow < lumaBlocksH) {
                                int32_t value = component->dcPrediction *
                                    (int32_t)decoder->quant[component->quantId].table[0];
                                value = (value / 8) + 128;
                                if (value < 0) value = 0;
                                if (value > 255) value = 255;
                                image->mean[blockRow][blockColumn] = (uint8_t)value;
                            }
                        }

                        /* Consume the AC coefficients to stay bit-exact. The
                         * zigzag index has to be tracked: a block that codes all
                         * 63 of them has no end-of-block symbol, so stopping only
                         * on EOB would run into the next block. */
                        int coefficient = 1;
                        while (coefficient < 64) {
                            int ac = jpegDecodeHuffman(decoder, acTable);
                            if (ac < 0)
                                return false;
                            int run = ac >> 4;
                            int size = ac & 0x0F;
                            if (size == 0) {
                                if (run != 15)
                                    break;       /* end of block */
                                coefficient += 16;   /* ZRL: 16 zeros */
                                continue;
                            }
                            coefficient += run + 1;
                            if (jpegReadBits(decoder, size) < 0)
                                return false;
                        }
                    }
                }
            }
            mcusSinceRestart++;
        }
    }
    image->complete = true;
    return true;
}

bool jpegDcDecode(const uint8_t* data, size_t size, JpegDcImage* image)
{
    if (data == NULL || image == NULL || size < 4)
        return false;
    image->blocksW = 0;
    image->blocksH = 0;
    image->complete = false;

    JpegDecoder decoder;
    memset(&decoder, 0, sizeof(decoder));
    decoder.data = data;
    decoder.size = size;

    if (data[0] != 0xFF || data[1] != 0xD8)   /* SOI */
        return false;
    decoder.pos = 2;

    bool sawFrame = false;
    for (;;) {
        int marker = jpegNextMarker(&decoder);
        if (marker < 0)
            return false;
        if (marker == 0xD9)                   /* EOI without a scan */
            return false;
        if (marker == 0xDA) {
            uint16_t length = 0;
            if (!jpegReadU16(&decoder, &length) || length < 2)
                return false;
            if (!sawFrame)
                return false;
            size_t headerEnd = decoder.pos - 2 + length;
            if (headerEnd > size)
                return false;
            if (!jpegParseScan(&decoder, length))
                return false;
            decoder.scan = data;
            decoder.scanSize = size;
            decoder.scanPos = headerEnd;
            return jpegDecodeScan(&decoder, image);
        }
        uint16_t length = 0;
        if (!jpegReadU16(&decoder, &length) || length < 2)
            return false;
        size_t payload = (size_t)(length - 2);
        if (decoder.pos + payload > size)
            return false;
        switch (marker) {
            case 0xC0: case 0xC1: case 0xC2: case 0xC3:
            case 0xC5: case 0xC6: case 0xC7:
            case 0xC9: case 0xCA: case 0xCB:
            case 0xCD: case 0xCE: case 0xCF:
                if (!jpegParseFrame(&decoder, length, (uint8_t)marker))
                    return false;
                sawFrame = true;
                break;
            case 0xC4:
                if (!jpegParseHuffman(&decoder, payload))
                    return false;
                break;
            case 0xDB:
                if (!jpegParseQuant(&decoder, payload))
                    return false;
                break;
            case 0xDD:
                if (payload != 2)
                    return false;
                {
                    uint16_t interval = 0;
                    if (!jpegReadU16(&decoder, &interval))
                        return false;
                    decoder.restartInterval = interval;
                }
                break;
            default:
                /* APPn, COM and friends carry nothing this decoder needs. */
                decoder.pos += payload;
                break;
        }
    }
}
