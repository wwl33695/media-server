#include "mov-reader.h"
#include "file-reader.h"
#include "mov-internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MOV_NULL MOV_TAG(0, 0, 0, 0)

struct mov_reader_t
{
	struct mov_t mov;
};

struct mov_parse_t
{
	uint32_t type;
	uint32_t parent;
	int(*parse)(struct mov_t* mov, const struct mov_box_t* box);
};

// 8.1.1 Media Data Box (p28)
static int mov_read_mdat(struct mov_t* mov, const struct mov_box_t* box)
{
	return file_reader_skip(mov->fp, box->size);
}

// 8.1.2 Free Space Box (p28)
static int mov_read_free(struct mov_t* mov, const struct mov_box_t* box)
{
	// Container: File or other box
	return file_reader_skip(mov->fp, box->size);
}

//static struct mov_stsd_t* mov_track_stsd_find(struct mov_track_t* track, uint32_t sample_description_index)
//{
//	size_t i;
//	for (i = 0; i < track->stsd_count; i++)
//	{
//		if (track->stsd[i].data_reference_index == sample_description_index)
//			return &track->stsd[i];
//	}
//	return NULL;
//}

static int mov_track_build(struct mov_track_t* track)
{
	size_t i, j, k;
	uint64_t n, chunk_offset;
	struct mov_stbl_t* stbl = &track->stbl;

	if (track->sample_count < 1)
		return 0;
	
	// sample offset
	assert(stbl->stsc_count > 0 && stbl->stco_count > 0);
	stbl->stsc[stbl->stsc_count].first_chunk = stbl->stco_count + 1; // fill stco count
	for (i = 0, n = 0; i < stbl->stsc_count; i++)
	{
		assert(stbl->stsc[i].first_chunk <= stbl->stco_count);
		for (j = stbl->stsc[i].first_chunk; j < stbl->stsc[i + 1].first_chunk; j++)
		{
			chunk_offset = stbl->stco[j-1]; // chunk start from 1
			for (k = 0; k < stbl->stsc[i].samples_per_chunk; k++, n++)
			{
				track->samples[n].sample_description_index = stbl->stsc[i].sample_description_index;
				track->samples[n].offset = chunk_offset;
				chunk_offset += track->samples[n].bytes;
				assert(track->samples[n].bytes > 0);
				assert(0 == n || track->samples[n-1].offset + track->samples[n-1].bytes <= track->samples[n].offset);
			}
		}
	}
	assert(n == track->sample_count);

	// edit list
	track->samples[0].dts = 0;
	track->samples[0].pts = 0;
	for (i = 0; i < track->elst_count; i++)
	{
		if (-1 == track->elst[i].media_time)
		{
			track->samples[0].dts = track->elst[i].segment_duration;
			track->samples[0].pts = track->samples[0].dts;
		}
	}

	// sample dts
	for (i = 0, n = 1; i < stbl->stts_count; i++)
	{
		for (j = 0; j < stbl->stts[i].sample_count; j++, n++)
		{
			track->samples[n].dts = track->samples[n - 1].dts + stbl->stts[i].sample_delta;
			track->samples[n].pts = track->samples[n].dts;
		}
	}
	assert(n - 1 == track->sample_count);

	// sample cts/pts
	for (i = 0, n = 0; i < stbl->ctts_count; i++)
	{
		for (j = 0; j < stbl->ctts[i].sample_count; j++, n++)
			track->samples[n].pts += stbl->ctts[i].sample_delta;
	}
	assert(0 == stbl->ctts_count || n == track->sample_count);

	return 0;
}

static int mov_fragment_build(struct mov_track_t* track)
{
	size_t i, j;
	for (i = 0; i < track->elst_count; i++)
	{
		if (-1 != track->elst[i].media_time)
			continue;
		
		for (j = 0; j < track->sample_count; j++)
		{
			track->samples[j].dts += track->elst[i].segment_duration;
			track->samples[j].pts += track->elst[i].segment_duration;
		}
	}
	return 0;
}

// 8.3.1 Track Box (p31)
// Box Type : ��trak�� 
// Container : Movie Box(��moov��) 
// Mandatory : Yes 
// Quantity : One or more
static int mov_read_trak(struct mov_t* mov, const struct mov_box_t* box)
{
	int r;
	void* p;
	p = realloc(mov->tracks, sizeof(struct mov_track_t) * (mov->track_count + 1));
	if (NULL == p) return ENOMEM;
	mov->tracks = p;
	mov->track_count += 1;
	mov->track = &mov->tracks[mov->track_count - 1];
	memset(mov->track, 0, sizeof(struct mov_track_t));

	r = mov_reader_box(mov, box);
	if (0 == r)
	{
		mov_track_build(mov->track);
		mov->track->end_dts = mov->track->sample_count ? mov->track->samples[mov->track->sample_count - 1].dts : 0;
	}

	return r;
}

static int mov_read_dref(struct mov_t* mov, const struct mov_box_t* box)
{
	uint32_t i, entry_count;
	file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	entry_count = file_reader_rb32(mov->fp);

	for (i = 0; i < entry_count; i++)
	{
		uint32_t size = file_reader_rb32(mov->fp);
		/*uint32_t type = */file_reader_rb32(mov->fp);
		/*uint32_t vern = */file_reader_rb32(mov->fp); /* version + flags */
		file_reader_skip(mov->fp, size-12);
	}

	(void)box;
	return 0;
}

static int mov_read_uuid(struct mov_t* mov, const struct mov_box_t* box)
{
	uint8_t usertype[16] = { 0 };
	if(box->size > 16) 
	{
		file_reader_read(mov->fp, usertype, sizeof(usertype));
		file_reader_skip(mov->fp, box->size - 16);
	}
	return 0;
}

static int mov_read_moof(struct mov_t* mov, const struct mov_box_t* box)
{
	mov->moof_offset = file_reader_tell(mov->fp) - 8 /*box size */;
	return mov_reader_box(mov, box);
}

static int mov_read_mehd(struct mov_t* mov, const struct mov_box_t* box)
{
	unsigned int version;
	uint64_t fragment_duration;
	version = file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */

	if (1 == version)
		fragment_duration = file_reader_rb64(mov->fp); /* fragment_duration*/
	else
		fragment_duration = file_reader_rb32(mov->fp); /* fragment_duration*/

	(void)box;
	assert(fragment_duration <= mov->mvhd.duration);
	return file_reader_error(mov->fp);
}

static int mov_read_mfhd(struct mov_t* mov, const struct mov_box_t* box)
{
	(void)box;
	file_reader_rb32(mov->fp); /* version & flags */
	file_reader_rb32(mov->fp); /* sequence_number */
	return file_reader_error(mov->fp);
}

// 8.8.12 Track fragment decode time (p76)
static int mov_read_tfdt(struct mov_t* mov, const struct mov_box_t* box)
{
	unsigned int version;
	version = file_reader_r8(mov->fp); /* version */
	file_reader_rb24(mov->fp); /* flags */
	if (1 == version)
		mov->track->end_dts = file_reader_rb64(mov->fp); /* baseMediaDecodeTime */
	else
		mov->track->end_dts = file_reader_rb32(mov->fp); /* baseMediaDecodeTime */
	return file_reader_error(mov->fp); (void)box;
}

// 8.8.11 Movie Fragment Random Access Offset Box (p75)
static int mov_read_mfro(struct mov_t* mov, const struct mov_box_t* box)
{
	uint32_t size;
	(void)box;
	file_reader_rb32(mov->fp); /* version & flags */
	size = file_reader_rb32(mov->fp); /* size */
	return file_reader_error(mov->fp);
}

static int mov_read_default(struct mov_t* mov, const struct mov_box_t* box)
{
	return mov_reader_box(mov, box);
}

static struct mov_parse_t s_mov_parse_table[] = {
	{ MOV_TAG('a', 'v', 'c', 'C'), MOV_NULL, mov_read_avcc }, // ISO/IEC 14496-15:2010(E) avcC
	{ MOV_TAG('c', 'o', '6', '4'), MOV_STBL, mov_read_stco },
	{ MOV_TAG('c', 't', 't', 's'), MOV_STBL, mov_read_ctts },
	{ MOV_TAG('d', 'i', 'n', 'f'), MOV_MINF, mov_read_default },
	{ MOV_TAG('d', 'r', 'e', 'f'), MOV_DINF, mov_read_dref },
	{ MOV_TAG('e', 'd', 't', 's'), MOV_TRAK, mov_read_default },
	{ MOV_TAG('e', 'l', 's', 't'), MOV_EDTS, mov_read_elst },
	{ MOV_TAG('e', 's', 'd', 's'), MOV_NULL, mov_read_esds }, // ISO/IEC 14496-14:2003(E) mp4a/mp4v/mp4s
	{ MOV_TAG('f', 'r', 'e', 'e'), MOV_NULL, mov_read_free },
	{ MOV_TAG('f', 't', 'y', 'p'), MOV_ROOT, mov_read_ftyp },
	{ MOV_TAG('h', 'd', 'l', 'r'), MOV_MDIA, mov_read_hdlr },
	{ MOV_TAG('h', 'v', 'c', 'C'), MOV_NULL, mov_read_hvcc }, // ISO/IEC 14496-15:2010(E) hvcC
	{ MOV_TAG('l', 'e', 'v', 'a'), MOV_MVEX, mov_read_leva },
	{ MOV_TAG('m', 'd', 'a', 't'), MOV_ROOT, mov_read_mdat },
	{ MOV_TAG('m', 'd', 'h', 'd'), MOV_MDIA, mov_read_mdhd },
	{ MOV_TAG('m', 'd', 'i', 'a'), MOV_TRAK, mov_read_default },
	{ MOV_TAG('m', 'e', 'h', 'd'), MOV_MVEX, mov_read_mehd },
	{ MOV_TAG('m', 'f', 'h', 'd'), MOV_MOOF, mov_read_mfhd },
	{ MOV_TAG('m', 'f', 'r', 'a'), MOV_ROOT, mov_read_default },
	{ MOV_TAG('m', 'f', 'r', 'o'), MOV_MFRA, mov_read_mfro },
	{ MOV_TAG('m', 'i', 'n', 'f'), MOV_MDIA, mov_read_default },
	{ MOV_TAG('m', 'o', 'o', 'v'), MOV_ROOT, mov_read_default },
	{ MOV_TAG('m', 'o', 'o', 'f'), MOV_ROOT, mov_read_moof },
	{ MOV_TAG('m', 'v', 'e', 'x'), MOV_MOOV, mov_read_default },
	{ MOV_TAG('m', 'v', 'h', 'd'), MOV_MOOV, mov_read_mvhd },
	{ MOV_TAG('s', 'i', 'd', 'x'), MOV_ROOT, mov_read_sidx },
	{ MOV_TAG('s', 'k', 'i', 'p'), MOV_NULL, mov_read_free },
	{ MOV_TAG('s', 'm', 'h', 'd'), MOV_MINF, mov_read_smhd },
	{ MOV_TAG('s', 't', 'b', 'l'), MOV_MINF, mov_read_default },
	{ MOV_TAG('s', 't', 'c', 'o'), MOV_STBL, mov_read_stco },
	{ MOV_TAG('s', 't', 's', 'c'), MOV_STBL, mov_read_stsc },
	{ MOV_TAG('s', 't', 's', 'd'), MOV_STBL, mov_read_stsd },
	{ MOV_TAG('s', 't', 's', 's'), MOV_STBL, mov_read_stss },
	{ MOV_TAG('s', 't', 's', 'z'), MOV_STBL, mov_read_stsz },
	{ MOV_TAG('s', 't', 't', 's'), MOV_STBL, mov_read_stts },
	{ MOV_TAG('s', 't', 'z', '2'), MOV_STBL, mov_read_stz2 },
	{ MOV_TAG('t', 'f', 'd', 't'), MOV_TRAF, mov_read_tfdt },
	{ MOV_TAG('t', 'f', 'h', 'd'), MOV_TRAF, mov_read_tfhd },
	{ MOV_TAG('t', 'f', 'r', 'a'), MOV_MFRA, mov_read_tfra },
	{ MOV_TAG('t', 'k', 'h', 'd'), MOV_TRAK, mov_read_tkhd },
	{ MOV_TAG('t', 'r', 'a', 'k'), MOV_MOOV, mov_read_trak },
	{ MOV_TAG('t', 'r', 'e', 'x'), MOV_MVEX, mov_read_trex },
	{ MOV_TAG('t', 'r', 'a', 'f'), MOV_MOOF, mov_read_default },
	{ MOV_TAG('t', 'r', 'u', 'n'), MOV_TRAF, mov_read_trun },
	{ MOV_TAG('u', 'u', 'i', 'd'), MOV_NULL, mov_read_uuid },
	{ MOV_TAG('v', 'm', 'h', 'd'), MOV_MINF, mov_read_vmhd },

	{ 0, 0, NULL } // last
};

int mov_reader_box(struct mov_t* mov, const struct mov_box_t* parent)
{
	int i;
	uint64_t bytes = 0;
	struct mov_box_t box;
	int (*parse)(struct mov_t* mov, const struct mov_box_t* box);

	while (bytes + 8 < parent->size && 0 == file_reader_error(mov->fp))
	{
		uint64_t n = 8;
		box.size = file_reader_rb32(mov->fp);
		box.type = file_reader_rb32(mov->fp);

		if (1 == box.size)
		{
			// unsigned int(64) largesize
			box.size = file_reader_rb64(mov->fp);
			n += 8;
		}
		else if (0 == box.size)
		{
			if (0 == box.type)
				break; // all done
			box.size = UINT64_MAX;
		}

		if (UINT64_MAX == box.size)
		{
			bytes = parent->size;
		}
		else
		{
			bytes += box.size;
			box.size -= n;
		}

		if (bytes > parent->size)
			return -1;

		for (i = 0, parse = NULL; s_mov_parse_table[i].type && !parse; i++)
		{
			if (s_mov_parse_table[i].type == box.type)
			{
				assert(MOV_NULL == s_mov_parse_table[i].parent
					|| s_mov_parse_table[i].parent == parent->type);
				parse = s_mov_parse_table[i].parse;
			}
		}

		if (NULL == parse)
		{
			file_reader_skip(mov->fp, box.size);
		}
		else
		{
			int r;
			uint64_t pos, pos2;
			pos = file_reader_tell(mov->fp);
			r = parse(mov, &box);
			assert(0 == r);
			if (0 != r) return r;
			pos2 = file_reader_tell(mov->fp);
			assert(pos2 - pos == box.size);
			file_reader_skip(mov->fp, box.size - (pos2 - pos));
		}
	}

	return 0;
}

static int mov_reader_init(struct mov_t* mov)
{
	int r;
	size_t i;
	struct mov_box_t box;
	struct mov_track_t* track;

	box.type = MOV_ROOT;
	box.size = UINT64_MAX;
	r = mov_reader_box(mov, &box);
	if (0 != r) return r;
	
	for (i = 0; i < mov->track_count; i++)
	{
		track = mov->tracks + i;
		mov_fragment_build(track);
		track->sample_offset = 0; // reset
	}
	return 0;
}

struct mov_reader_t* mov_reader_create(const char* file)
{
	struct mov_reader_t* reader;
	reader = (struct mov_reader_t*)calloc(1, sizeof(*reader));
	if (NULL == reader)
		return NULL;

	// ISO/IEC 14496-12:2012(E) 4.3.1 Definition (p17)
	// Files with no file-type box should be read as if they contained an FTYP box 
	// with Major_brand='mp41', minor_version=0, and the single compatible brand 'mp41'.
	reader->mov.ftyp.major_brand = MOV_BRAND_MP41;
	reader->mov.ftyp.minor_version = 0;
	reader->mov.ftyp.brands_count = 0;
	reader->mov.header = 0;

	reader->mov.fp = file_reader_create(file);
	if (NULL == reader->mov.fp || 0 != mov_reader_init(&reader->mov))
	{
		mov_reader_destroy(reader);
		return NULL;
	}
	return reader;
}

#define FREE(p) do { if(p) free(p); } while(0)

void mov_reader_destroy(struct mov_reader_t* reader)
{
	size_t i;
	file_reader_destroy(&reader->mov.fp);
	for (i = 0; i < reader->mov.track_count; i++)
	{
		FREE(reader->mov.tracks[i].extra_data);
		FREE(reader->mov.tracks[i].stsd);
		FREE(reader->mov.tracks[i].elst);
		FREE(reader->mov.tracks[i].samples);
		FREE(reader->mov.tracks[i].stbl.stco);
		FREE(reader->mov.tracks[i].stbl.stsc);
		FREE(reader->mov.tracks[i].stbl.stss);
		FREE(reader->mov.tracks[i].stbl.stts);
		FREE(reader->mov.tracks[i].stbl.ctts);
	}
	FREE(reader->mov.tracks);
	free(reader);
}

static struct mov_track_t* mov_reader_next(struct mov_reader_t* reader)
{
	size_t i;
	struct mov_track_t* track = NULL;
	struct mov_track_t* track2;

	for (i = 0; i < reader->mov.track_count; i++)
	{
		track2 = &reader->mov.tracks[i];
		assert(track2->sample_offset <= track2->sample_count);
		if (track2->sample_offset >= track2->sample_count)
			continue;

		//if (NULL == track || track->samples[track->sample_offset].dts > track2->samples[track2->sample_offset].dts)
		if (NULL == track || track->samples[track->sample_offset].offset > track2->samples[track2->sample_offset].offset)
		{
			track = track2;
		}
	}

	return track;
}

int mov_reader_read(struct mov_reader_t* reader, void* buffer, size_t bytes, mov_reader_onread onread, void* param)
{
	struct mov_track_t* track;
	struct mov_sample_t* sample;

	track = mov_reader_next(reader);
	if (NULL == track || 0 == track->mdhd.timescale)
	{
		return 0; // EOF
	}

	assert(track->sample_offset < track->sample_count);
	sample = &track->samples[track->sample_offset];
	if (bytes < sample->bytes)
		return ENOMEM;

	if (0 != file_reader_seek(reader->mov.fp, sample->offset))
	{
		return -1;
	}

	if (sample->bytes != file_reader_read(reader->mov.fp, buffer, sample->bytes))
	{
		return file_reader_error(reader->mov.fp);
	}

	track->sample_offset++; //mark as read
	onread(param, track->tkhd.track_ID, buffer, sample->bytes, sample->pts * 1000 / track->mdhd.timescale, sample->dts * 1000 / track->mdhd.timescale);
	return 1;
}

int mov_reader_seek(struct mov_reader_t* reader, int64_t* timestamp)
{
	size_t i, j;
	int64_t pts;
	struct mov_track_t* track;
	struct mov_sample_t* sample;

	for (i = 0; i < reader->mov.track_count; i++)
	{
		track = &reader->mov.tracks[i];
		if (track->stbl.stss_count > 0)
		{
			// TODO: qsearch
			for (j = 0; j < track->stbl.stss_count; j++)
			{
				if (track->stbl.stss[j] < 1 || track->stbl.stss[j] > track->sample_count)
				{
					// start from 1
					assert(0);
					break;
				}

				sample = &track->samples[track->stbl.stss[j] - 1];
				pts = sample->pts * 1000 / track->mdhd.timescale;
				if (*timestamp < pts)
				{
					track->sample_offset = track->stbl.stss[j] - 1;
					if (track->sample_offset > 0)
						track->sample_offset -= 1;
					sample = &track->samples[track->sample_offset];
					*timestamp = pts; // FIXME

					// change other track offset
					for (j = 1; j < reader->mov.track_count; j++)
					{
						track = &reader->mov.tracks[(i + j) % reader->mov.track_count];
						for (track->sample_offset = 0; track->sample_offset < track->sample_count; ++track->sample_offset)
						{
							if (track->samples[track->sample_offset].offset > sample->offset)
							{
								pts = track->samples[track->sample_offset].pts * 1000 / track->mdhd.timescale;
								if (pts < *timestamp)
									*timestamp = pts; // mimimum timestamp
								break;
							}
						}
					}
					return 0;
				}
			}
		}
	}

	return -1;
}

int mov_reader_getinfo(struct mov_reader_t* reader, mov_reader_onvideo onvideo, mov_reader_onaudio onaudio, void* param)
{
	size_t i, j;
	struct mov_stsd_t* stsd;
	struct mov_track_t* track;

	for (i = 0; i < reader->mov.track_count; i++)
	{
		track = &reader->mov.tracks[i];
		for (j = 0; j < track->stsd_count && j < 1 /* only the first */; j++)
		{
			stsd = &track->stsd[j];
			switch (track->handler_type)
			{
			case MOV_VIDEO:
				onvideo(param, track->tkhd.track_ID, stsd->object_type_indication, stsd->u.visual.width, stsd->u.visual.height, track->extra_data, track->extra_data_size);
				break;

			case MOV_AUDIO:
				onaudio(param, track->tkhd.track_ID, stsd->object_type_indication, stsd->u.audio.channelcount, stsd->u.audio.samplesize, stsd->u.audio.samplerate >> 16, track->extra_data, track->extra_data_size);
				break;

			default:
				break;
			}
		}	
	}
	return 0;
}

uint64_t mov_reader_getduration(struct mov_reader_t* reader)
{
	return reader->mov.mvhd.duration * 1000 / reader->mov.mvhd.timescale;
}
