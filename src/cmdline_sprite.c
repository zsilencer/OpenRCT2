#include <lodepng/lodepng.h>
#include "cmdline.h"
#include "drawing/drawing.h"
#include "util/util.h"

typedef struct {
	uint32 num_entries;
	uint32 total_size;
} rct_sprite_file_header;

typedef struct { uint8 b, g, r, a; } rct_sprite_file_palette_entry;

rct_sprite_file_palette_entry spriteFilePalette[256];
static rct_sprite_file_palette_entry _standardPalette[256];

rct_sprite_file_header spriteFileHeader;
rct_g1_element *spriteFileEntries;
uint8 *spriteFileData;

void sprite_file_load_palette(int spriteIndex)
{
	rct_g1_element *g1 = &spriteFileEntries[spriteIndex];
	int numPaletteEntries = g1->width;
	uint8* src = g1->offset;
	rct_sprite_file_palette_entry *destPaletteEntry = &spriteFilePalette[g1->x_offset];
	for (; numPaletteEntries > 0; numPaletteEntries--) {
		destPaletteEntry->b = src[0];
		destPaletteEntry->g = src[1];
		destPaletteEntry->r = src[2];
		src += 3;
		destPaletteEntry++;
	}
}

void sprite_entries_make_absolute()
{
	for (uint32 i = 0; i < spriteFileHeader.num_entries; i++)
		spriteFileEntries[i].offset += (int)spriteFileData;
}

void sprite_entries_make_relative()
{
	for (uint32 i = 0; i < spriteFileHeader.num_entries; i++)
		spriteFileEntries[i].offset -= (int)spriteFileData;
}

bool sprite_file_open(const char *path)
{
	FILE *file;

	file = fopen(path, "rb");
	if (file == NULL)
		return false;

	if (fread(&spriteFileHeader, sizeof(rct_sprite_file_header), 1, file) != 1) {
		fclose(file);
		return false;
	}

	if (spriteFileHeader.num_entries > 0) {
		int entryTableSize = spriteFileHeader.num_entries * sizeof(rct_g1_element);

		spriteFileEntries = malloc(entryTableSize);
		if (fread(spriteFileEntries, entryTableSize, 1, file) != 1) {
			fclose(file);
			return false;
		}

		spriteFileData = malloc(spriteFileHeader.total_size);
		if (fread(spriteFileData, spriteFileHeader.total_size, 1, file) != 1) {
			fclose(file);
			return false;
		}

		sprite_entries_make_absolute();
	}

	fclose(file);
	return true;
}

bool sprite_file_save(const char *path)
{
	FILE *file = fopen(path, "wb");
	if (file == NULL)
		return false;
	
	if (fwrite(&spriteFileHeader, sizeof(rct_sprite_file_header), 1, file) != 1) {
		fclose(file);
		return false;
	}

	if (spriteFileHeader.num_entries > 0) {
		sprite_entries_make_relative();

		int entryTableSize = spriteFileHeader.num_entries * sizeof(rct_g1_element);

		if (fwrite(spriteFileEntries, entryTableSize, 1, file) != 1) {
			sprite_entries_make_absolute();
			fclose(file);
			return false;
		} else {
			sprite_entries_make_absolute();
		}

		if (fwrite(spriteFileData, spriteFileHeader.total_size, 1, file) != 1) {
			fclose(file);
			return false;
		}
	}

	fclose(file);
	return true;
}

void sprite_file_close()
{
	free(spriteFileEntries);
	free(spriteFileData);
}

bool sprite_file_export(int spriteIndex, const char *outPath)
{
	rct_g1_element *spriteHeader;
	rct_drawpixelinfo dpi;
	uint8 *pixels;
	int pixelBufferSize;

	spriteHeader = &spriteFileEntries[spriteIndex];
	pixelBufferSize = spriteHeader->width * spriteHeader->height;
	pixels = malloc(pixelBufferSize);
	memset(pixels, 0, pixelBufferSize);

	dpi.bits = pixels;
	dpi.x = 0;
	dpi.y = 0;
	dpi.width = spriteHeader->width;
	dpi.height = spriteHeader->height;
	dpi.pitch = 0;
	dpi.zoom_level = 0;

	memcpy(spriteFilePalette, _standardPalette, 256 * 4);
	gfx_rle_sprite_to_buffer(spriteHeader->offset, pixels, (uint8*)spriteFilePalette, &dpi, IMAGE_TYPE_NO_BACKGROUND, 0, spriteHeader->height, 0, spriteHeader->width);

	LodePNGState pngState;
	unsigned int pngError;
	unsigned char* pngData;
	size_t pngSize;

	lodepng_state_init(&pngState);
	pngState.info_raw.colortype = LCT_PALETTE;
	lodepng_palette_add(&pngState.info_raw, 0, 0, 0, 0);
	for (int i = 1; i < 256; i++) {
		lodepng_palette_add(
			&pngState.info_raw,
			spriteFilePalette[i].r,
			spriteFilePalette[i].g,
			spriteFilePalette[i].b,
			255
		);
	}

	pngError = lodepng_encode(&pngData, &pngSize, pixels, spriteHeader->width, spriteHeader->height, &pngState);
	if (pngError != 0) {
		fprintf(stderr, "Error creating PNG data, %u: %s", pngError, lodepng_error_text(pngError));
		return false;
	} else {
		lodepng_save_file(pngData, pngSize, outPath);
		free(pngData);
		return true;
	}
}

int get_palette_index(uint32 colour)
{
	uint8 *rgba = (uint8*)(&colour);
	
	if (rgba[3] < 128)
		return -1;

	for (int i = 10; i < 246; i++) {
		if (spriteFilePalette[i].r != rgba[0]) continue;
		if (spriteFilePalette[i].g != rgba[1]) continue;
		if (spriteFilePalette[i].b != rgba[2]) continue;
		return i;
	}

	return -1;
}

typedef struct {
	uint8 num_pixels;
	uint8 offset_x;
} rle_code;

bool sprite_file_import(const char *path, rct_g1_element *outElement, uint8 **outBuffer, int *outBufferLength)
{
	unsigned char *pixels;
	unsigned int width, height;
	unsigned int pngError;
	bool paletted = false;

	memcpy(spriteFilePalette, _standardPalette, 256 * 4);

	if(paletted){
		unsigned char* buffer1;
		size_t buffer1size;
		pngError = lodepng_load_file(&buffer1, &buffer1size, path);
		LodePNGState state;
		lodepng_state_init(&state);
		state.decoder.color_convert = 0;
		state.info_raw.colortype = LCT_PALETTE;
		if(!pngError) pngError = lodepng_decode(&pixels, &width, &height, &state, buffer1, buffer1size);
		lodepng_state_cleanup(&state);
		free(buffer1);
	}else{
		pngError = lodepng_decode_file(&pixels, &width, &height, path, LCT_RGBA, 8);
	}

	if (pngError != 0) {
		fprintf(stderr, "Error creating PNG data, %u: %s", pngError, lodepng_error_text(pngError));
		return false;
	}

	if (width > 256 || height > 256) {
		fprintf(stderr, "Only images 256x256 or less are supported.");
		free(pixels);
		return false;
	}

	uint8 *buffer = malloc((height * 2) + (width * height * 16));
	uint16 *yOffsets = (uint16*)buffer;
	uint8 *src = pixels;
	uint8 *dst = buffer + (height * 2);
	
	for (unsigned int y = 0; y < height; y++) {
		rle_code *previousCode, *currentCode;

		yOffsets[y] = (dst - buffer);

		previousCode = NULL;
		currentCode = (rle_code*)dst;
		dst += 2;
		int startX = 0;
		int pixels = 0;
		bool pushRun = false;
		for (unsigned int x = 0; x < width; x++) {
			uint8 paletteIndex = paletted ? *src : get_palette_index(*((uint32*)src));
			src += paletted ? 1 : 4;
			if (paletteIndex == -1) {
				if (pixels != 0) {
					x--;
					src -= paletted ? 1 : 4;
					pushRun = true;
				}
			} else {
				if (pixels == 0)
					startX = x;
				pixels++;
				*dst++ = (uint8)paletteIndex;
			}
			if (pixels == 127 || x == width - 1)
				pushRun = true;

			if (pushRun) {
				if (pixels > 0) {
					previousCode = currentCode;
					currentCode->num_pixels = pixels;
					currentCode->offset_x = startX;

					if (x == width - 1)
						currentCode->num_pixels |= 0x80;

					currentCode = (rle_code*)dst;
					dst += 2;
				} else {
					if (previousCode == NULL) {
						currentCode->num_pixels = 0x80;
						currentCode->offset_x = 0;
					} else {
						previousCode->num_pixels |= 0x80;
						dst -= 2;
					}
				}
				startX = 0;
				pixels = 0;
				pushRun = false;
			}
		}
	}
	free(pixels);

	int bufferLength = (int)(dst - buffer);
	buffer = realloc(buffer, bufferLength);

	outElement->offset = buffer;
	outElement->width = width;
	outElement->height = height;
	outElement->flags = G1_FLAG_RLE_COMPRESSION;
	outElement->x_offset = 0;
	outElement->y_offset = 0;
	outElement->zoomed_offset = 0;

	*outBuffer = buffer;
	*outBufferLength = bufferLength;
	return true;
}

int cmdline_for_sprite(const char **argv, int argc)
{
	if (argc == 0)
		return -1;

	if (_strcmpi(argv[0], "details") == 0) {
		if (argc < 2) {
			fprintf(stderr, "usage: sprite details <spritefile> [idx]\n");
			return -1;
		} else if (argc == 2) {
			const char *spriteFilePath = argv[1];

			if (!sprite_file_open(spriteFilePath)) {
				fprintf(stderr, "Unable to open input sprite file.\n");
				return -1;
			}

			printf("sprites: %d\n", spriteFileHeader.num_entries);
			printf("data size: %d\n", spriteFileHeader.total_size);

			sprite_file_close();
			return 1;
		} else {
			const char *spriteFilePath = argv[1];
			int spriteIndex = atoi(argv[2]);

			if (!sprite_file_open(spriteFilePath)) {
				fprintf(stderr, "Unable to open input sprite file.\n");
				return -1;
			}

			if (spriteIndex < 0 || spriteIndex >= (int)spriteFileHeader.num_entries) {
				sprite_file_close();
				fprintf(stderr, "Sprite #%d does not exist in sprite file.\n", spriteIndex);
				return -1;
			}

			rct_g1_element *g1 = &spriteFileEntries[spriteIndex];
			printf("width: %d\n", g1->width);
			printf("height: %d\n", g1->height);
			printf("x offset: %d\n", g1->x_offset);
			printf("y offset: %d\n", g1->y_offset);
			printf("data offset: 0x%X\n", g1->offset);

			sprite_file_close();
			return 1;
		}
	} else if (_strcmpi(argv[0], "export") == 0) {
		if (argc < 4) {
			fprintf(stderr, "usage: sprite export <spritefile> <idx> <output>\n");
			return -1;
		}

		const char *spriteFilePath = argv[1];
		int spriteIndex = atoi(argv[2]);
		const char *outputPath = argv[3];

		if (!sprite_file_open(spriteFilePath)) {
			fprintf(stderr, "Unable to open input sprite file.\n");
			return -1;
		}

		if (spriteIndex < 0 || spriteIndex >= (int)spriteFileHeader.num_entries) {
			fprintf(stderr, "Sprite #%d does not exist in sprite file.\n", spriteIndex);
			return -1;
		}

		if (!sprite_file_export(spriteIndex, outputPath)) {
			sprite_file_close();
			return -1;
		}

		sprite_file_close();
		return 1;
	} else if (_strcmpi(argv[0], "create") == 0) {
		if (argc < 2) {
			fprintf(stderr, "usage: sprite create <spritefile>\n");
			return -1;
		}

		const char *spriteFilePath = argv[1];

		spriteFileHeader.num_entries = 0;
		spriteFileHeader.total_size = 0;
		sprite_file_save(spriteFilePath);

		sprite_file_close();
		return 1;
	} else if (_strcmpi(argv[0], "append") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: sprite append <spritefile> <input>\n");
			return -1;
		}

		const char *spriteFilePath = argv[1];
		const char *imagePath = argv[2];

		rct_g1_element spriteElement;
		uint8 *buffer;
		int bufferLength;
		if (!sprite_file_import(imagePath, &spriteElement, &buffer, &bufferLength))
			return -1;

		if (!sprite_file_open(spriteFilePath)) {
			fprintf(stderr, "Unable to open input sprite file.\n");
			return -1;
		}
		
		spriteFileHeader.num_entries++;
		spriteFileHeader.total_size += bufferLength;
		spriteFileEntries = realloc(spriteFileEntries, spriteFileHeader.num_entries * sizeof(rct_g1_element));
		spriteFileData = realloc(spriteFileData, spriteFileHeader.total_size);
		spriteFileEntries[spriteFileHeader.num_entries - 1] = spriteElement;
		memcpy(spriteFileData + (spriteFileHeader.total_size - bufferLength), buffer, bufferLength);
		spriteFileEntries[spriteFileHeader.num_entries - 1].offset = spriteFileData + (spriteFileHeader.total_size - bufferLength);
		
		free(buffer);
		if (!sprite_file_save(spriteFilePath))
			return -1;

		return 1;
	} else {
		fprintf(stderr, "Unknown sprite command.");
		return -1;
	}
}

static rct_sprite_file_palette_entry _standardPalette[256] = {
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 35, 35, 23, 255 },
	{ 51, 51, 35, 255 },
	{ 67, 67, 47, 255 },
	{ 83, 83, 63, 255 },
	{ 99, 99, 75, 255 },
	{ 115, 115, 91, 255 },
	{ 131, 131, 111, 255 },
	{ 151, 151, 131, 255 },
	{ 175, 175, 159, 255 },
	{ 195, 195, 183, 255 },
	{ 219, 219, 211, 255 },
	{ 243, 243, 239, 255 },
	{ 0, 47, 51, 255 },
	{ 0, 59, 63, 255 },
	{ 11, 75, 79, 255 },
	{ 19, 91, 91, 255 },
	{ 31, 107, 107, 255 },
	{ 47, 123, 119, 255 },
	{ 59, 139, 135, 255 },
	{ 79, 155, 151, 255 },
	{ 95, 175, 167, 255 },
	{ 115, 191, 187, 255 },
	{ 139, 207, 203, 255 },
	{ 163, 227, 223, 255 },
	{ 7, 43, 67, 255 },
	{ 11, 59, 87, 255 },
	{ 23, 75, 111, 255 },
	{ 31, 87, 127, 255 },
	{ 39, 99, 143, 255 },
	{ 51, 115, 159, 255 },
	{ 67, 131, 179, 255 },
	{ 87, 151, 191, 255 },
	{ 111, 175, 203, 255 },
	{ 135, 199, 219, 255 },
	{ 163, 219, 231, 255 },
	{ 195, 239, 247, 255 },
	{ 0, 27, 71, 255 },
	{ 0, 43, 95, 255 },
	{ 0, 63, 119, 255 },
	{ 7, 83, 143, 255 },
	{ 7, 111, 167, 255 },
	{ 15, 139, 191, 255 },
	{ 19, 167, 215, 255 },
	{ 27, 203, 243, 255 },
	{ 47, 231, 255, 255 },
	{ 95, 243, 255, 255 },
	{ 143, 251, 255, 255 },
	{ 195, 255, 255, 255 },
	{ 0, 0, 35, 255 },
	{ 0, 0, 79, 255 },
	{ 7, 7, 95, 255 },
	{ 15, 15, 111, 255 },
	{ 27, 27, 127, 255 },
	{ 39, 39, 143, 255 },
	{ 59, 59, 163, 255 },
	{ 79, 79, 179, 255 },
	{ 103, 103, 199, 255 },
	{ 127, 127, 215, 255 },
	{ 159, 159, 235, 255 },
	{ 191, 191, 255, 255 },
	{ 19, 51, 27, 255 },
	{ 23, 63, 35, 255 },
	{ 31, 79, 47, 255 },
	{ 39, 95, 59, 255 },
	{ 43, 111, 71, 255 },
	{ 51, 127, 87, 255 },
	{ 59, 143, 99, 255 },
	{ 67, 155, 115, 255 },
	{ 75, 171, 131, 255 },
	{ 83, 187, 147, 255 },
	{ 95, 203, 163, 255 },
	{ 103, 219, 183, 255 },
	{ 27, 55, 31, 255 },
	{ 35, 71, 47, 255 },
	{ 43, 83, 59, 255 },
	{ 55, 99, 75, 255 },
	{ 67, 111, 91, 255 },
	{ 79, 135, 111, 255 },
	{ 95, 159, 135, 255 },
	{ 111, 183, 159, 255 },
	{ 127, 207, 183, 255 },
	{ 147, 219, 195, 255 },
	{ 167, 231, 207, 255 },
	{ 191, 247, 223, 255 },
	{ 0, 63, 15, 255 },
	{ 0, 83, 19, 255 },
	{ 0, 103, 23, 255 },
	{ 0, 123, 31, 255 },
	{ 7, 143, 39, 255 },
	{ 23, 159, 55, 255 },
	{ 39, 175, 71, 255 },
	{ 63, 191, 91, 255 },
	{ 87, 207, 111, 255 },
	{ 115, 223, 139, 255 },
	{ 143, 239, 163, 255 },
	{ 179, 255, 195, 255 },
	{ 19, 43, 79, 255 },
	{ 27, 55, 99, 255 },
	{ 43, 71, 119, 255 },
	{ 59, 87, 139, 255 },
	{ 67, 99, 167, 255 },
	{ 83, 115, 187, 255 },
	{ 99, 131, 207, 255 },
	{ 115, 151, 215, 255 },
	{ 131, 171, 227, 255 },
	{ 151, 191, 239, 255 },
	{ 171, 207, 247, 255 },
	{ 195, 227, 255, 255 },
	{ 55, 19, 15, 255 },
	{ 87, 43, 39, 255 },
	{ 103, 55, 51, 255 },
	{ 119, 67, 63, 255 },
	{ 139, 83, 83, 255 },
	{ 155, 99, 99, 255 },
	{ 175, 119, 119, 255 },
	{ 191, 139, 139, 255 },
	{ 207, 159, 159, 255 },
	{ 223, 183, 183, 255 },
	{ 239, 211, 211, 255 },
	{ 255, 239, 239, 255 },
	{ 111, 27, 0, 255 },
	{ 151, 39, 0, 255 },
	{ 167, 51, 7, 255 },
	{ 187, 67, 15, 255 },
	{ 203, 83, 27, 255 },
	{ 223, 103, 43, 255 },
	{ 227, 135, 67, 255 },
	{ 231, 163, 91, 255 },
	{ 239, 187, 119, 255 },
	{ 243, 211, 143, 255 },
	{ 251, 231, 175, 255 },
	{ 255, 247, 215, 255 },
	{ 15, 43, 11, 255 },
	{ 23, 55, 15, 255 },
	{ 31, 71, 23, 255 },
	{ 43, 83, 35, 255 },
	{ 59, 99, 47, 255 },
	{ 75, 115, 59, 255 },
	{ 95, 135, 79, 255 },
	{ 119, 155, 99, 255 },
	{ 139, 175, 123, 255 },
	{ 167, 199, 147, 255 },
	{ 195, 219, 175, 255 },
	{ 223, 243, 207, 255 },
	{ 95, 0, 63, 255 },
	{ 115, 7, 75, 255 },
	{ 127, 15, 83, 255 },
	{ 143, 31, 95, 255 },
	{ 155, 43, 107, 255 },
	{ 171, 63, 123, 255 },
	{ 187, 83, 135, 255 },
	{ 199, 103, 155, 255 },
	{ 215, 127, 171, 255 },
	{ 231, 155, 191, 255 },
	{ 243, 195, 215, 255 },
	{ 255, 235, 243, 255 },
	{ 0, 0, 63, 255 },
	{ 0, 0, 87, 255 },
	{ 0, 0, 115, 255 },
	{ 0, 0, 143, 255 },
	{ 0, 0, 171, 255 },
	{ 0, 0, 199, 255 },
	{ 0, 7, 227, 255 },
	{ 0, 7, 255, 255 },
	{ 67, 79, 255, 255 },
	{ 115, 123, 255, 255 },
	{ 163, 171, 255, 255 },
	{ 215, 219, 255, 255 },
	{ 0, 39, 79, 255 },
	{ 0, 51, 111, 255 },
	{ 0, 63, 147, 255 },
	{ 0, 71, 183, 255 },
	{ 0, 79, 219, 255 },
	{ 0, 83, 255, 255 },
	{ 23, 111, 255, 255 },
	{ 51, 139, 255, 255 },
	{ 79, 163, 255, 255 },
	{ 107, 183, 255, 255 },
	{ 135, 203, 255, 255 },
	{ 163, 219, 255, 255 },
	{ 47, 51, 0, 255 },
	{ 55, 63, 0, 255 },
	{ 67, 75, 0, 255 },
	{ 79, 87, 0, 255 },
	{ 99, 107, 7, 255 },
	{ 119, 127, 23, 255 },
	{ 143, 147, 43, 255 },
	{ 163, 167, 71, 255 },
	{ 187, 187, 99, 255 },
	{ 207, 207, 131, 255 },
	{ 231, 231, 171, 255 },
	{ 255, 255, 207, 255 },
	{ 27, 0, 63, 255 },
	{ 51, 0, 103, 255 },
	{ 63, 11, 123, 255 },
	{ 79, 23, 143, 255 },
	{ 95, 31, 163, 255 },
	{ 111, 39, 183, 255 },
	{ 143, 59, 219, 255 },
	{ 171, 91, 239, 255 },
	{ 187, 119, 243, 255 },
	{ 203, 151, 247, 255 },
	{ 223, 183, 251, 255 },
	{ 239, 215, 255, 255 },
	{ 0, 19, 39, 255 },
	{ 7, 31, 55, 255 },
	{ 15, 47, 71, 255 },
	{ 31, 63, 91, 255 },
	{ 51, 83, 107, 255 },
	{ 75, 103, 123, 255 },
	{ 107, 127, 143, 255 },
	{ 127, 147, 163, 255 },
	{ 147, 171, 187, 255 },
	{ 171, 195, 207, 255 },
	{ 195, 219, 231, 255 },
	{ 223, 243, 255, 255 },
	{ 75, 75, 55, 255 },
	{ 0, 183, 255, 255 },
	{ 0, 219, 255, 255 },
	{ 0, 255, 255, 255 },
	{ 99, 107, 7, 255 },
	{ 99, 107, 7, 255 },
	{ 135, 143, 39, 255 },
	{ 123, 131, 27, 255 },
	{ 99, 107, 7, 255 },
	{ 151, 155, 55, 255 },
	{ 151, 155, 55, 255 },
	{ 227, 227, 155, 255 },
	{ 203, 203, 115, 255 },
	{ 151, 155, 55, 255 },
	{ 91, 91, 67, 255 },
	{ 107, 107, 83, 255 },
	{ 123, 123, 99, 255 },
	{ 47, 47, 47, 255 },
	{ 47, 47, 47, 255 },
	{ 47, 71, 87, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 },
	{ 0, 0, 0, 255 }
};