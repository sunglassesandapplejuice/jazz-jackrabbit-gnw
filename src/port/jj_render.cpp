/*
 * Harness only (option -R): renders the music modules (*.PSM) of the pack
 * with xmp, the player of OpenJazz, for tools/jj_pack.py.
 *
 * For each module, <dir>/<NAME>.MUS is the piece in the GWMU format of
 * jj_music.c (version 2: with its loop start), mono (left + right) / 2,
 * signed 16 bit PCM or IMA ADPCM.
 *
 * The song is played twice: the file is the first pass followed by the
 * first TAIL_SECONDS of the second one, in which the notes held at the end
 * of the song still sound (they would be cut by a jump back to the start),
 * and the loop goes back to TAIL_SECONDS after the restart position of the
 * song, where the second pass and the first one are the same again.
 */
#ifndef JJ_DEVICE
#include <stdio.h>
#include <string.h>
#include <vector>
#include <xmp.h>
#include "jj_platform.h"
#include "jj_pack.h"

// longest pass: songs that never end are cut there
#define MAX_SECONDS 600
#define TAIL_SECONDS 2

static const int16_t ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
#define ADPCM_BLOCK 1024

/* blocks of ADPCM_BLOCK samples: int16 predictor, u8 step index, u8 0,
 * 4 bit codes, low nibble first (jj_music.c) */
static void adpcm_encode(const int16_t *s, long n, std::vector<uint8_t> &out)
{
	for (long b = 0; b < n; b += ADPCM_BLOCK) {
		long len = n - b < ADPCM_BLOCK ? n - b : ADPCM_BLOCK;
		const int16_t *blk = s + b;
		int pred = blk[0];
		int idx = 0;
		if (len > 1) {
			int d = blk[1] - blk[0];
			if (d < 0) d = -d;
			while (idx < 88 && ima_step[idx] < d) idx++;
		}
		out.push_back((uint8_t)(pred & 0xFF));
		out.push_back((uint8_t)((pred >> 8) & 0xFF));
		out.push_back((uint8_t)idx);
		out.push_back(0);
		uint8_t byte = 0;
		for (long i = 0; i < len; i++) {
			int step = ima_step[idx];
			int diff = blk[i] - pred;
			int code = 0;
			if (diff < 0) { code = 8; diff = -diff; }
			int dq = step >> 3;
			if (diff >= step) { code |= 4; diff -= step; dq += step; }
			step >>= 1;
			if (diff >= step) { code |= 2; diff -= step; dq += step; }
			step >>= 1;
			if (diff >= step) { code |= 1; dq += step; }
			pred = (code & 8) ? pred - dq : pred + dq;
			if (pred > 32767) pred = 32767;
			if (pred < -32768) pred = -32768;
			idx += ima_index[code & 7];
			if (idx < 0) idx = 0;
			if (idx > 88) idx = 88;
			if (i & 1) {
				out.push_back((uint8_t)(byte | (code << 4)));
			} else {
				byte = (uint8_t)code;
			}
		}
		if (len & 1)
			out.push_back(byte);
	}
}

static void put16(std::vector<uint8_t> &v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static void put32(std::vector<uint8_t> &v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }

static bool render_one(const char *name, const uint8_t *data, uint32_t size, const char *dir, int rate, bool adpcm)
{
	xmp_context ctx = xmp_create_context();
	if (!ctx)
		return false;
	if (xmp_load_module_from_memory(ctx, data, (long)size) != 0) {
		printf("render: %s: not a module\n", name);
		xmp_free_context(ctx);
		return false;
	}
	xmp_start_player(ctx, rate, XMP_FORMAT_MONO);
	// offline: the best interpolation (the game used "nearest" on handhelds)
	xmp_set_player(ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
	xmp_set_player(ctx, XMP_PLAYER_AMP, 2); // like sound.cpp
	xmp_set_player(ctx, XMP_PLAYER_VOLUME, 100);

	std::vector<int16_t> out;
	out.reserve((size_t)rate * 120);
	long pass1 = -1, pass2 = -1, pass3 = -1;
	const size_t max_samples = (size_t)rate * MAX_SECONDS;
	struct xmp_frame_info fi;
	while (out.size() < 3 * max_samples) {
		if (xmp_play_frame(ctx) != 0)
			break;
		xmp_get_frame_info(ctx, &fi);
		if (fi.loop_count >= 1 && pass1 < 0)
			pass1 = (long)out.size();
		if (fi.loop_count >= 2 && pass2 < 0)
			pass2 = (long)out.size();
		if (fi.loop_count >= 3 && pass3 < 0)
			pass3 = (long)out.size();
		// enough after the third pass starts for the checks
		if (pass3 > 0 && (long)out.size() >= pass3 + (long)rate * (TAIL_SECONDS + 1))
			break;
		const int16_t *b = (const int16_t *)fi.buffer;
		int n = fi.buffer_size / 2;
		for (int i = 0; i < n; i++)
			out.push_back(b[i]);
		if (pass1 < 0 && out.size() >= max_samples)
			break;
	}
	xmp_end_player(ctx);
	xmp_release_module(ctx);
	xmp_free_context(ctx);

	// rms difference between what follows the end of the file and the loop
	auto diff = [&](long end, long loop) -> double {
		double err = 0;
		long n = 0;
		for (long i = 0; i < rate / 2 && end + i < (long)out.size() && loop + i < end; i++, n++) {
			double d = out[end + i] - out[loop + i];
			err += d * d;
		}
		return n ? __builtin_sqrt(err / n) : 1e9;
	};
	const long tail_max = (long)rate * TAIL_SECONDS;
	long total, loop_start;
	double err = 0;
	const char *how = "no end";
	if (pass1 > 0 && pass2 > pass1) {
		// loop on the second pass of the song (restart position .. end)
		long body = pass2 - pass1;
		long restart = pass1 - body;
		if (restart < 0) restart = 0;
		long tail = tail_max < body ? tail_max : body;
		if (restart + tail > pass1) tail = pass1 - restart;
		total = pass1 + tail;
		loop_start = restart + tail;
		err = diff(total, loop_start);
		how = "pass 1";
		if (err > 100 && pass3 > pass2) {
			// the song changes on the way (tempo, volume...): loop on the
			// third pass, which is like the second one
			long body3 = pass3 - pass2;
			long tail3 = tail_max < body3 ? tail_max : body3;
			long total3 = pass2 + tail3;
			long loop3 = pass2 - body3 + tail3;
			double err3 = diff(total3, loop3);
			if (err3 < err) {
				total = total3;
				loop_start = loop3;
				err = err3;
				how = "pass 2";
			}
		}
	} else {
		total = (long)(out.size() < max_samples ? out.size() : max_samples);
		loop_start = 0;
	}

	char stem[32];
	snprintf(stem, sizeof(stem), "%s", name);
	char *dot = strchr(stem, '.');
	if (dot) *dot = '\0';
	std::vector<uint8_t> mus;
	mus.insert(mus.end(), { 'G', 'W', 'M', 'U' });
	put16(mus, 2);                 // version
	put16(mus, adpcm ? 1 : 0);     // codec
	put32(mus, (uint32_t)rate);
	put32(mus, (uint32_t)total);
	put32(mus, (uint32_t)loop_start);
	if (adpcm) {
		adpcm_encode(out.data(), total, mus);
	} else {
		for (long i = 0; i < total; i++)
			put16(mus, (uint16_t)out[i]);
	}
	char path[600];
	snprintf(path, sizeof(path), "%s/%s.MUS", dir, stem);
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	fwrite(mus.data(), 1, mus.size(), f);
	fclose(f);

	printf("render: %s: %.1f s, loop at %.1f s (%s, rms diff %.1f)\n", name, total / (double)rate,
		loop_start / (double)rate, how, err);
	return true;
}

extern "C" int jj_render_music(const char *dir, int rate, bool adpcm)
{
	int count = 0;
	for (uint32_t i = 0; i < jj_pack_count(); i++) {
		const char *name = jj_pack_name(i);
		size_t len = strlen(name);
		if (len < 5 || strcmp(name + len - 4, ".PSM") != 0)
			continue;
		const uint8_t *data;
		uint32_t size;
		if (jj_pack_find(name, &data, &size) && render_one(name, data, size, dir, rate, adpcm))
			count++;
	}
	printf("render: %d pieces\n", count);
	return count;
}
#endif
