#include <gtest/gtest.h>

#include "llhls_chunklist.h"
#include "llhls_keyframe_start_aligner.h"

#include <base/modules/data_format/cue_event/cue_event.h>
#include <base/modules/marker/marker.h>
#include <zlib.h>

#include <atomic>
#include <thread>

namespace
{
	std::shared_ptr<MediaTrack> CreateVideoTrack()
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(1);
		track->SetMediaType(cmn::MediaType::Video);
		track->SetPublicName("video");
		track->SetVariantName("video");
		return track;
	}

	std::shared_ptr<LLHlsChunklist> CreateChunklist(const std::shared_ptr<MediaTrack> &track)
	{
		return std::make_shared<LLHlsChunklist>("chunklist_1_video_key_llhls.m3u8", track,
												10,	 // segment count
												6,	 // target duration
												0.5, // part target duration
												"init_1_video_key_llhls.m4s", true);
	}

	// Simulates a segment packaged against the given track version, the way
	// LLHlsStream::OnMediaChunkUpdated stamps partial infos from the storage segment
	void AppendSegment(const std::shared_ptr<LLHlsChunklist> &chunklist, uint32_t sequence, uint32_t track_version, const ov::String &map_uri, bool discontinuity_point = false, const ov::String &codecs = "", const ov::String &upcoming_map_uri = "", std::optional<uint32_t> upcoming_track_version = std::nullopt)
	{
		auto url = ov::String::FormatString("seg_1_%u_video_key_llhls.m4s", sequence);
		chunklist->CreateSegmentInfo(LLHlsChunklist::SegmentInfo(sequence, url));

		auto partial_url = ov::String::FormatString("part_1_%u_0_video_key_llhls.m4s", sequence);
		auto next_partial_url = ov::String::FormatString("part_1_%u_1_video_key_llhls.m4s", sequence);
		auto partial_info = LLHlsChunklist::SegmentInfo(0, sequence * 6000, 6.0, 1000, partial_url, next_partial_url, true, true);
		partial_info.SetTrackVersion(track_version);
		partial_info.SetMapUri(map_uri);
		partial_info.SetCodecsParameter(codecs);
		partial_info.SetUpcomingMapUri(upcoming_map_uri);
		if (upcoming_track_version.has_value() == true)
		{
			partial_info.SetUpcomingTrackVersion(upcoming_track_version.value());
		}
		if (discontinuity_point == true)
		{
			partial_info.SetDiscontinuity();
		}

		chunklist->AppendPartialSegmentInfo(sequence, partial_info);
	}

	const ov::String kInitialMapUri = "init_1_video_key_llhls.m4s";
	const ov::String kSecondMapUri = "init_1_video_key_v2_llhls.m4s";

	// A cbcs Widevine key. Distinct key_ids produce distinct EXT-X-KEY tags (the
	// KEYID attribute), the way a DRM key rotation registers each content version.
	bmff::CencProperty MakeCencProperty(const char *key_id_hex)
	{
		bmff::CencProperty cenc_property;
		cenc_property.scheme = bmff::CencProtectScheme::Cbcs;
		cenc_property.key_id = ov::Hex::Decode(key_id_hex);
		cenc_property.key = ov::Hex::Decode("16cf4232a86364b519e1982a27d90087");
		cenc_property.iv = ov::Hex::Decode("572547f914e34dc68ba9ba9ef91d4c4a");
		cenc_property.pssh_box_list.push_back(bmff::PsshBox("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed", {cenc_property.key_id}, nullptr));
		return cenc_property;
	}

	const char *kKeyIdA = "572543f964e34dc68ba9ba9ef91d4c4a";
	const char *kKeyIdB = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

	ov::String KeyIdAttribute(const char *key_id_hex)
	{
		return ov::String::FormatString("KEYID=0x%s", ov::String(key_id_hex).UpperCaseString().CStr());
	}

	ov::String Gunzip(const std::shared_ptr<const ov::Data> &data)
	{
		z_stream zs{};
		inflateInit2(&zs, 15 | 16);
		zs.next_in = const_cast<Bytef *>(data->GetDataAs<Bytef>());
		zs.avail_in = static_cast<uInt>(data->GetLength());

		std::string text;
		char buffer[4096];
		int result;
		do
		{
			zs.next_out = reinterpret_cast<Bytef *>(buffer);
			zs.avail_out = sizeof(buffer);
			result = inflate(&zs, Z_NO_FLUSH);
			text.append(buffer, sizeof(buffer) - zs.avail_out);
		} while (result == Z_OK);
		inflateEnd(&zs);

		EXPECT_EQ(result, Z_STREAM_END);
		return ov::String(text.c_str(), text.size());
	}

	ov::String DefaultView(const std::shared_ptr<LLHlsChunklist> &chunklist)
	{
		return chunklist->ToString("", false, false, true);
	}

	int CountOccurrences(const ov::String &text, const ov::String &needle)
	{
		int count = 0;
		for (auto index = text.IndexOf(needle.CStr()); index != -1; index = text.IndexOf(needle.CStr(), index + 1))
		{
			count++;
		}
		return count;
	}
} // namespace

TEST(LLHlsChunklist, NoDiscontinuityTagsByDefault)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	EXPECT_EQ(playlist.IndexOf("#EXT-X-DISCONTINUITY"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_llhls.m4s\""), -1);
}

TEST(LLHlsChunklist, TrackVersionChangeInsertsTagAndNewMap)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	// The next segment was packaged against a new track configuration. A real track
	// change flags the boundary segment as a discontinuity point (the storage marks
	// it), which is what the chunklist turns into an EXT-X-DISCONTINUITY.
	AppendSegment(chunklist, 2, 2, kSecondMapUri, true);

	auto playlist = chunklist->ToString("", false, false, false);

	auto discontinuity_index = playlist.IndexOf("#EXT-X-DISCONTINUITY\n");
	EXPECT_NE(discontinuity_index, -1);

	// The initial map leads the playlist, the new map follows the discontinuity
	auto initial_map_index = playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_llhls.m4s\"");
	auto new_map_index = playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_v2_llhls.m4s\"");
	EXPECT_NE(initial_map_index, -1);
	EXPECT_NE(new_map_index, -1);
	EXPECT_LT(initial_map_index, discontinuity_index);
	EXPECT_LT(discontinuity_index, new_map_index);

	// The new map precedes the segment it applies to
	EXPECT_LT(new_map_index, playlist.IndexOf("seg_1_2_video_key_llhls.m4s"));

	EXPECT_NE(playlist.IndexOf("#EXT-X-DISCONTINUITY-SEQUENCE:0\n"), -1);
}

TEST(LLHlsChunklist, DiscontinuitySequenceCountsRemovedTags)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 2, kSecondMapUri, true);
	AppendSegment(chunklist, 2, 2, kSecondMapUri);
	AppendSegment(chunklist, 3, 2, kSecondMapUri);

	// Removing the unflagged first segment keeps the sequence number
	chunklist->RemoveSegmentInfo(0);
	auto playlist = chunklist->ToString("", false, false, false);
	EXPECT_NE(playlist.IndexOf("#EXT-X-DISCONTINUITY-SEQUENCE:0\n"), -1);
	// The tag stays while its segment is the first of the playlist
	EXPECT_NE(playlist.IndexOf("#EXT-X-DISCONTINUITY\n"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_v2_llhls.m4s\""), -1);

	// Removing the flagged segment removes its tag and increments the sequence
	chunklist->RemoveSegmentInfo(1);
	playlist = chunklist->ToString("", false, false, false);
	EXPECT_NE(playlist.IndexOf("#EXT-X-DISCONTINUITY-SEQUENCE:1\n"), -1);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-DISCONTINUITY\n"), -1);
}

TEST(LLHlsChunklist, PropagatedBoundaryFlagsWithoutVersionChange)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);

	// Another track of the stream changed; this track was cut at the boundary but
	// keeps its own configuration and map
	AppendSegment(chunklist, 1, 1, kInitialMapUri, true);

	auto playlist = chunklist->ToString("", false, false, false);

	auto discontinuity_index = playlist.IndexOf("#EXT-X-DISCONTINUITY\n");
	EXPECT_NE(discontinuity_index, -1);
	EXPECT_LT(discontinuity_index, playlist.IndexOf("seg_1_1_video_key_llhls.m4s"));

	// The map does not change, so only one EXT-X-MAP appears
	auto first_map_index = playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_llhls.m4s\"");
	EXPECT_NE(first_map_index, -1);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-MAP", first_map_index + 1), -1);
}

TEST(LLHlsChunklist, CompletionWithoutAChunkCarriesItsMarkers)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	// The segment closed on what was already stored, so its marker never rode
	// a partial; the completion is the only chance to hand it over
	auto cue = CueEvent::Create(CueEvent::CueType::OUT, 8000, 0, false);
	ASSERT_NE(cue, nullptr);
	auto marker = Marker::CreateMarker(cmn::BitstreamFormat::CUE, 12000, 12000, cue->Serialize());
	ASSERT_NE(marker, nullptr);
	chunklist->CompleteSegmentInfo(1, "part_1_2_0_video_key_llhls.m4s", kInitialMapUri, std::nullopt, {marker});

	auto segment = chunklist->GetSegmentInfo(1);
	ASSERT_NE(segment, nullptr);
	ASSERT_EQ(segment->GetMarkers().size(), 1u);
	EXPECT_EQ(segment->GetMarkers().front(), marker);

	// A completion that carries nothing leaves the markers alone
	chunklist->CompleteSegmentInfo(1, "part_1_2_0_video_key_llhls.m4s", kInitialMapUri);
	EXPECT_EQ(segment->GetMarkers().size(), 1u);
}

TEST(LLHlsChunklist, BoundaryCutHintsUpcomingMap)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	// Boundary cut: segment 1 is completed and the next partial opens a new map
	chunklist->CompleteSegmentInfo(1, "part_1_2_0_video_key_llhls.m4s", kSecondMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	// The upcoming map is hinted ahead of the part that will use it
	auto map_hint_index = playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP,URI=\"init_1_video_key_v2_llhls.m4s\"");
	auto part_hint_index = playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"part_1_2_0_video_key_llhls.m4s\"");
	EXPECT_NE(map_hint_index, -1);
	EXPECT_NE(part_hint_index, -1);
	EXPECT_LT(map_hint_index, part_hint_index);

	// Once the first partial of the new domain is listed the map appears inline
	AppendSegment(chunklist, 2, 2, kSecondMapUri);
	playlist = chunklist->ToString("", false, false, false);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"part_1_2_1_video_key_llhls.m4s\""), -1);
}

TEST(LLHlsChunklist, UpcomingMapHintSetDirectly)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	// The boundary flush completed segment 1 through the normal chunk path; the
	// stream sets the upcoming map once the new initialization section is stored
	AppendSegment(chunklist, 1, 1, kInitialMapUri);
	chunklist->SetUpcomingMapUri(kSecondMapUri);

	auto playlist = chunklist->ToString("", false, false, false);
	EXPECT_NE(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP,URI=\"init_1_video_key_v2_llhls.m4s\""), -1);

	// The first partial of the new domain retires the map hint
	AppendSegment(chunklist, 2, 2, kSecondMapUri);
	playlist = chunklist->ToString("", false, false, false);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP"), -1);
}

TEST(LLHlsChunklist, PropagatedBoundaryCutDoesNotHintMap)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	// A cut propagated from another track keeps this track's map
	chunklist->CompleteSegmentInfo(1, "part_1_2_0_video_key_llhls.m4s", kInitialMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	// No map change, so no map hint; the redirected part hint stays
	EXPECT_EQ(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"part_1_2_0_video_key_llhls.m4s\""), -1);
}

TEST(LLHlsChunklist, CodecsUnionFollowsListing)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri, false, "avc1.42c00d");
	AppendSegment(chunklist, 1, 2, kSecondMapUri, false, "avc1.640029");

	// Both versions are listed
	EXPECT_STREQ(chunklist->GetListedCodecsUnion().CStr(), "avc1.42c00d,avc1.640029");

	// The old version left the listing, its codecs leave the listed union but
	// stay in the learned history for dumped output
	chunklist->RemoveSegmentInfo(0);
	EXPECT_STREQ(chunklist->GetListedCodecsUnion().CStr(), "avc1.640029");
	EXPECT_STREQ(chunklist->GetAllCodecsUnion().CStr(), "avc1.42c00d,avc1.640029");
}

TEST(LLHlsChunklist, VersionChangeBeforeFirstSegmentHasNoTag)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	// The track changed before anything was published; the first segment simply
	// starts with the new configuration
	AppendSegment(chunklist, 0, 2, kSecondMapUri);
	AppendSegment(chunklist, 1, 2, kSecondMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	EXPECT_EQ(playlist.IndexOf("#EXT-X-DISCONTINUITY"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_v2_llhls.m4s\""), -1);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_llhls.m4s\""), -1);
}

TEST(LLHlsChunklist, SingleKeyEmittedOnce)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	// Exactly one EXT-X-KEY for the single content version
	auto first_key = playlist.IndexOf("#EXT-X-KEY:");
	EXPECT_NE(first_key, -1);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-KEY:", first_key + 1), -1);
}

TEST(LLHlsChunklist, KeyRotationEmitsNewKeyWithoutDiscontinuity)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	chunklist->EnableCenc(2, MakeCencProperty(kKeyIdB));

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	// A key rotation: a new content version and map, but no discontinuity flag
	AppendSegment(chunklist, 2, 2, kSecondMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	// A key rotation is seamless: no discontinuity tag
	EXPECT_EQ(playlist.IndexOf("#EXT-X-DISCONTINUITY"), -1);

	// The new initialization section still switches in
	EXPECT_NE(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_v2_llhls.m4s\""), -1);

	// Both keys are advertised, the new one after the old one
	auto key_a = ov::String::FormatString("KEYID=0x%s", ov::String(kKeyIdA).UpperCaseString().CStr());
	auto key_b = ov::String::FormatString("KEYID=0x%s", ov::String(kKeyIdB).UpperCaseString().CStr());
	auto key_a_index = playlist.IndexOf(key_a.CStr());
	auto key_b_index = playlist.IndexOf(key_b.CStr());
	EXPECT_NE(key_a_index, -1);
	EXPECT_NE(key_b_index, -1);
	EXPECT_LT(key_a_index, key_b_index);

	// The new key precedes the segment it applies to
	EXPECT_LT(key_b_index, playlist.IndexOf("seg_1_2_video_key_llhls.m4s"));
}

TEST(LLHlsChunklist, KeyRotationHintsUpcomingMapOnCompletingChunk)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	chunklist->EnableCenc(2, MakeCencProperty(kKeyIdB));

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	// The chunk completing segment 1 carries the map and version of the rotated
	// content that opens segment 2, the way LLHlsStream stamps it after applying
	// the rotation
	AppendSegment(chunklist, 1, 1, kInitialMapUri, false, "", kSecondMapUri, 2);

	auto playlist = chunklist->ToString("", false, false, false);

	// The hinted partial belongs to the new version, so its key and map are announced
	// ahead of it, in that order
	auto key_b_index = playlist.IndexOf(KeyIdAttribute(kKeyIdB).CStr());
	auto map_hint_index = playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP,URI=\"init_1_video_key_v2_llhls.m4s\"");
	auto part_hint_index = playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"part_1_1_1_video_key_llhls.m4s\"");
	EXPECT_NE(key_b_index, -1);
	EXPECT_NE(map_hint_index, -1);
	EXPECT_NE(part_hint_index, -1);
	EXPECT_LT(key_b_index, map_hint_index);
	EXPECT_LT(map_hint_index, part_hint_index);
	EXPECT_EQ(CountOccurrences(playlist, KeyIdAttribute(kKeyIdB)), 1);

	// The listed segments still carry the old key and map only
	EXPECT_LT(playlist.IndexOf(KeyIdAttribute(kKeyIdA).CStr()), playlist.IndexOf("seg_1_0_video_key_llhls.m4s"));
	EXPECT_LT(playlist.IndexOf("seg_1_1_video_key_llhls.m4s"), key_b_index);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_v2_llhls.m4s\""), -1);

	// The first partial of the rotated version retires the hint and lists the map and
	// key inline, once
	AppendSegment(chunklist, 2, 2, kSecondMapUri);
	playlist = chunklist->ToString("", false, false, false);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-MAP:URI=\"init_1_video_key_v2_llhls.m4s\""), -1);
	EXPECT_EQ(CountOccurrences(playlist, KeyIdAttribute(kKeyIdB)), 1);
	EXPECT_LT(playlist.IndexOf(KeyIdAttribute(kKeyIdB).CStr()), playlist.IndexOf("seg_1_2_video_key_llhls.m4s"));
	EXPECT_EQ(playlist.IndexOf("#EXT-X-DISCONTINUITY"), -1);
}

TEST(LLHlsChunklist, UpcomingVersionKeepingTheKeyHintsMapWithoutRepeatingKey)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	chunklist->EnableCenc(2, MakeCencProperty(kKeyIdA));

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	// A new version that keeps the current key: the map is hinted, the key is not repeated
	AppendSegment(chunklist, 1, 1, kInitialMapUri, false, "", kSecondMapUri, 2);

	auto playlist = chunklist->ToString("", false, false, false);
	EXPECT_NE(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP,URI=\"init_1_video_key_v2_llhls.m4s\""), -1);
	EXPECT_EQ(CountOccurrences(playlist, KeyIdAttribute(kKeyIdA)), 1);
}

TEST(LLHlsChunklist, ClearUpcomingVersionHintsMapWithMethodNone)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	// The track changed to a codec CENC cannot encrypt, so the upcoming version is
	// produced in the clear and registered before its map is hinted
	chunklist->EnableCenc(2, bmff::CencProperty());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri, false, "", kSecondMapUri, 2);

	auto playlist = chunklist->ToString("", false, false, false);

	// The update that hints the clear map ends the scope of the key ahead of the hint
	auto none_index = playlist.IndexOf("#EXT-X-KEY:METHOD=NONE");
	auto map_hint_index = playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP,URI=\"init_1_video_key_v2_llhls.m4s\"");
	EXPECT_NE(none_index, -1);
	EXPECT_NE(map_hint_index, -1);
	EXPECT_LT(playlist.IndexOf("seg_1_1_video_key_llhls.m4s"), none_index);
	EXPECT_LT(none_index, map_hint_index);
	EXPECT_EQ(CountOccurrences(playlist, ov::String("#EXT-X-KEY:METHOD=NONE")), 1);

	// Once the clear version is listed, METHOD=NONE precedes its segment, once
	AppendSegment(chunklist, 2, 2, kSecondMapUri, true);
	playlist = chunklist->ToString("", false, false, false);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP"), -1);
	EXPECT_EQ(CountOccurrences(playlist, ov::String("#EXT-X-KEY:METHOD=NONE")), 1);
	EXPECT_LT(playlist.IndexOf("#EXT-X-KEY:METHOD=NONE"), playlist.IndexOf("seg_1_2_video_key_llhls.m4s"));
}

TEST(LLHlsChunklist, CompletingChunkWithUnchangedMapDoesNotHintMap)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	// No rotation: the completing chunk names the map already in effect
	AppendSegment(chunklist, 1, 1, kInitialMapUri, false, "", kInitialMapUri);

	auto playlist = chunklist->ToString("", false, false, false);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP"), -1);
	EXPECT_NE(playlist.IndexOf("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"part_1_1_1_video_key_llhls.m4s\""), -1);
}

TEST(LLHlsChunklist, TrackChangeKeepingTheKeyDoesNotRepeatIt)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	// A scheduled channel changes tracks often; those versions keep the current key
	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	chunklist->EnableCenc(2, MakeCencProperty(kKeyIdA));
	// Then a rotation brings a new key
	chunklist->EnableCenc(3, MakeCencProperty(kKeyIdB));

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	// Track change: new version and map, discontinuity, same key
	AppendSegment(chunklist, 1, 2, kSecondMapUri, true);
	// Key rotation: new version and map, no discontinuity, new key
	AppendSegment(chunklist, 2, 3, "init_1_video_key_v3_llhls.m4s");

	auto playlist = chunklist->ToString("", false, false, false);

	auto key_a = ov::String::FormatString("KEYID=0x%s", ov::String(kKeyIdA).UpperCaseString().CStr());
	auto key_b = ov::String::FormatString("KEYID=0x%s", ov::String(kKeyIdB).UpperCaseString().CStr());

	// The unchanged key is advertised once, not repeated for the track change
	auto first_key_a = playlist.IndexOf(key_a.CStr());
	EXPECT_NE(first_key_a, -1);
	EXPECT_EQ(playlist.IndexOf(key_a.CStr(), first_key_a + 1), -1);

	// The rotated key appears before the segment it applies to
	auto key_b_index = playlist.IndexOf(key_b.CStr());
	EXPECT_NE(key_b_index, -1);
	EXPECT_LT(first_key_a, key_b_index);
	EXPECT_LT(key_b_index, playlist.IndexOf("seg_1_2_video_key_llhls.m4s"));

	// Only the track change carries a discontinuity, the rotation does not
	auto discontinuity_index = playlist.IndexOf("#EXT-X-DISCONTINUITY\n");
	EXPECT_NE(discontinuity_index, -1);
	EXPECT_EQ(playlist.IndexOf("#EXT-X-DISCONTINUITY\n", discontinuity_index + 1), -1);
	EXPECT_LT(discontinuity_index, key_b_index);
}

TEST(LLHlsChunklist, DumpedOutputKeepsEveryVersionKey)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	// Dumped output lists every segment ever produced
	chunklist->SaveOldSegmentInfo(true);

	// More versions than any fixed retention would keep. A version whose key was dropped
	// while still listed would inherit the previous EXT-X-KEY and fail to decrypt.
	constexpr uint32_t kVersions = 70;
	for (uint32_t version = 1; version <= kVersions; version++)
	{
		auto key_id = ov::String::FormatString("%032x", version);
		chunklist->EnableCenc(version, MakeCencProperty(key_id.CStr()));
		AppendSegment(chunklist, version - 1, version, ov::String::FormatString("init_1_video_key_v%u_llhls.m4s", version));
	}

	// The live window slides past the early segments; they stay listed in the dumped view
	for (uint32_t sequence = 0; sequence + 10 < kVersions; sequence++)
	{
		chunklist->RemoveSegmentInfo(sequence);
	}

	auto playlist = chunklist->ToString("", false, false, false, true, 0);

	for (uint32_t version : {static_cast<uint32_t>(1), kVersions / 2, kVersions})
	{
		auto key_id = ov::String::FormatString("%032x", version).UpperCaseString();
		auto expected = ov::String::FormatString("KEYID=0x%s", key_id.CStr());
		EXPECT_NE(playlist.IndexOf(expected.CStr()), -1) << "missing key of version " << version;
	}
}

TEST(LLHlsChunklist, HasMapUriMatchesEmittedMap)
{
	auto with_map = CreateChunklist(CreateVideoTrack());
	AppendSegment(with_map, 0, 1, kInitialMapUri);

	EXPECT_TRUE(with_map->HasMapUri());
	EXPECT_NE(with_map->ToString("", false, false, false).IndexOf("#EXT-X-MAP"), -1);
	EXPECT_NE(with_map->ToString("", false, false, false, true, 0).IndexOf("#EXT-X-MAP"), -1);

	// A track without an initialization segment (e.g. WebVTT)
	auto subtitle_track = std::make_shared<MediaTrack>();
	subtitle_track->SetId(2);
	subtitle_track->SetMediaType(cmn::MediaType::Subtitle);
	subtitle_track->SetPublicName("subtitle");
	auto without_map = std::make_shared<LLHlsChunklist>("chunklist_2_subtitle_key_llhls.m3u8", subtitle_track, 10, 6, 0.5, "", true);
	AppendSegment(without_map, 0, 1, "");

	EXPECT_FALSE(without_map->HasMapUri());
	EXPECT_EQ(without_map->ToString("", false, false, false).IndexOf("#EXT-X-MAP"), -1);
	EXPECT_EQ(without_map->ToString("", false, false, false, true, 0).IndexOf("#EXT-X-MAP"), -1);
}

TEST(LLHlsChunklist, ClearVersionEndsTheKeyScope)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	// The track changed to a codec CENC cannot encrypt, so this version is produced in
	// the clear. Its segments must not stay covered by the preceding key.
	chunklist->EnableCenc(2, bmff::CencProperty());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 2, kSecondMapUri, true);

	auto playlist = chunklist->ToString("", false, false, false);

	auto key_id = ov::String::FormatString("KEYID=0x%s", ov::String(kKeyIdA).UpperCaseString().CStr());
	auto key_index = playlist.IndexOf(key_id.CStr());
	auto none_index = playlist.IndexOf("#EXT-X-KEY:METHOD=NONE");

	EXPECT_NE(key_index, -1);
	EXPECT_NE(none_index, -1);
	EXPECT_LT(key_index, none_index);

	// The scope ends before the clear segment
	EXPECT_LT(none_index, playlist.IndexOf("seg_1_1_video_key_llhls.m4s"));
}

TEST(LLHlsChunklist, ClearOnlyPlaylistHasNoKeyTag)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	// A stream without DRM registers its versions with a scheme of None. There is no key
	// scope to end, so the playlist carries no key tag at all.
	chunklist->EnableCenc(1, bmff::CencProperty());
	chunklist->EnableCenc(2, bmff::CencProperty());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 2, kSecondMapUri, true);

	auto playlist = chunklist->ToString("", false, false, false);

	EXPECT_EQ(playlist.IndexOf("#EXT-X-KEY"), -1);
}

TEST(LLHlsChunklist, WindowStartingOnClearVersionHasNoKeyTag)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	chunklist->EnableCenc(1, MakeCencProperty(kKeyIdA));
	chunklist->EnableCenc(2, bmff::CencProperty());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 2, kSecondMapUri, true);

	// The protected segment leaves the window, so the key it belonged to is no longer
	// advertised and there is nothing left to end
	chunklist->RemoveSegmentInfo(0);

	auto playlist = chunklist->ToString("", false, false, false);

	EXPECT_EQ(playlist.IndexOf("#EXT-X-KEY"), -1);
}

TEST(LLHlsChunklist, NoKeyTagWhenCencDisabled)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());

	AppendSegment(chunklist, 0, 1, kInitialMapUri);
	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	auto playlist = chunklist->ToString("", false, false, false);

	// No key registered, so the playlist carries no EXT-X-KEY
	EXPECT_EQ(playlist.IndexOf("#EXT-X-KEY:"), -1);
}

TEST(LLHlsChunklist, DefaultViewFollowsEveryChange)
{
	// One chunklist is requested after every change, the other is rendered once at the end
	auto requested = CreateChunklist(CreateVideoTrack());
	auto reference = CreateChunklist(CreateVideoTrack());
	auto both = {requested, reference};

	for (auto &chunklist : both)
	{
		AppendSegment(chunklist, 0, 1, kInitialMapUri);
	}
	EXPECT_NE(DefaultView(requested).IndexOf("seg_1_0_video_key_llhls.m4s"), -1);

	for (auto &chunklist : both)
	{
		AppendSegment(chunklist, 1, 2, kSecondMapUri, true);
	}
	EXPECT_NE(DefaultView(requested).IndexOf("#EXT-X-DISCONTINUITY\n"), -1);

	for (auto &chunklist : both)
	{
		chunklist->SetUpcomingMapUri("init_1_video_key_v3_llhls.m4s", 3);
	}
	EXPECT_NE(DefaultView(requested).IndexOf("#EXT-X-PRELOAD-HINT:TYPE=MAP,URI=\"init_1_video_key_v3_llhls.m4s\""), -1);

	for (auto &chunklist : both)
	{
		chunklist->RemoveSegmentInfo(0);
	}
	EXPECT_EQ(DefaultView(requested).IndexOf("seg_1_0_video_key_llhls.m4s"), -1);

	for (auto &chunklist : both)
	{
		AppendSegment(chunklist, 2, 3, "init_1_video_key_v3_llhls.m4s", true);
		chunklist->SetEndList();
	}
	EXPECT_NE(DefaultView(requested).IndexOf("#EXT-X-ENDLIST"), -1);

	EXPECT_EQ(DefaultView(requested), DefaultView(reference));
}

TEST(LLHlsChunklist, DefaultViewListsThePartGetLastSequenceNumberReported)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	std::atomic<bool> done = false;
	int checked = 0;
	int missing = 0;

	// The newest listed part, not the preload hint that names the next one
	auto lists_part = [](const ov::String &view, int64_t msn, int64_t psn) {
		off_t last_part = -1;
		for (auto index = view.IndexOf("#EXT-X-PART:"); index != -1; index = view.IndexOf("#EXT-X-PART:", index + 1))
		{
			last_part = index;
		}
		int64_t listed_msn = -1, listed_psn = -1;
		if (last_part >= 0)
		{
			auto uri = view.Substring(view.IndexOf("part_1_", last_part));
			std::sscanf(uri.CStr(), "part_1_%" SCNd64 "_%" SCNd64, &listed_msn, &listed_psn);
		}
		return listed_msn > msn || (listed_msn == msn && listed_psn >= psn);
	};

	// A blocking request is answered once GetLastSequenceNumber reports its part, so the views must list it
	std::thread reader([&]() {
		int64_t last_msn = -1, last_psn = -1;
		while (done == false)
		{
			int64_t msn, psn;
			chunklist->GetLastSequenceNumber(msn, psn);
			if (msn < 0 || (msn == last_msn && psn == last_psn))
			{
				continue;
			}
			last_msn = msn;
			last_psn = psn;

			if (lists_part(DefaultView(chunklist), msn, psn) == false ||
				lists_part(Gunzip(chunklist->ToGzipData("", false, false, true)), msn, psn) == false)
			{
				missing++;
			}
			checked++;
		}
	});

	for (uint32_t sequence = 0; sequence < 200; sequence++)
	{
		chunklist->CreateSegmentInfo(LLHlsChunklist::SegmentInfo(sequence, ov::String::FormatString("seg_1_%u_video_key_llhls.m4s", sequence)));
		for (uint32_t part = 0; part < 6; part++)
		{
			auto url = ov::String::FormatString("part_1_%u_%u_video_key_llhls.m4s", sequence, part);
			auto next_url = ov::String::FormatString("part_1_%u_%u_video_key_llhls.m4s", part == 5 ? sequence + 1 : sequence, part == 5 ? 0 : part + 1);
			chunklist->AppendPartialSegmentInfo(sequence, LLHlsChunklist::SegmentInfo(part, (sequence * 6 + part) * 1000, 1.0, 1000, url, next_url, part == 0, part == 5));
		}
	}

	done = true;
	reader.join();

	EXPECT_GT(checked, 0);
	EXPECT_EQ(missing, 0);
}

TEST(LLHlsChunklist, CompletedSegmentListsItsWholeDuration)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	chunklist->CreateSegmentInfo(LLHlsChunklist::SegmentInfo(0, "seg_1_0_video_key_llhls.m4s"));

	// Completed by its last partial
	chunklist->AppendPartialSegmentInfo(0, LLHlsChunklist::SegmentInfo(0, 0, 2.5, 1000, "part_1_0_0_video_key_llhls.m4s", "part_1_0_1_video_key_llhls.m4s", true, false));
	chunklist->AppendPartialSegmentInfo(0, LLHlsChunklist::SegmentInfo(1, 2500, 3.5, 1000, "part_1_0_1_video_key_llhls.m4s", "part_1_1_0_video_key_llhls.m4s", false, true));

	// Completed without a partial, at a track change boundary
	chunklist->CreateSegmentInfo(LLHlsChunklist::SegmentInfo(1, "seg_1_1_video_key_llhls.m4s"));
	chunklist->AppendPartialSegmentInfo(1, LLHlsChunklist::SegmentInfo(0, 6000, 1.25, 1000, "part_1_1_0_video_key_llhls.m4s", "part_1_1_1_video_key_llhls.m4s", true, false));
	chunklist->CompleteSegmentInfo(1, "part_1_2_0_video_key_llhls.m4s", kInitialMapUri);

	for (const auto &playlist : {DefaultView(chunklist), chunklist->ToString("session=1", false, false, true), chunklist->ToString("", false, true, false)})
	{
		EXPECT_NE(playlist.IndexOf("#EXTINF:6.000000,\nseg_1_0_video_key_llhls.m4s"), -1);
		EXPECT_NE(playlist.IndexOf("#EXTINF:1.250000,\nseg_1_1_video_key_llhls.m4s"), -1);
		EXPECT_EQ(CountOccurrences(playlist, "#EXT-X-PROGRAM-DATE-TIME:"), 2);
	}
}

TEST(LLHlsChunklist, GzipDefaultViewIsTheTextUntilTheNextChange)
{
	auto chunklist = CreateChunklist(CreateVideoTrack());
	AppendSegment(chunklist, 0, 1, kInitialMapUri);

	auto gzip = chunklist->ToGzipData("", false, false, true);
	EXPECT_EQ(Gunzip(gzip), DefaultView(chunklist));
	EXPECT_EQ(chunklist->ToGzipData("", false, false, true), gzip);

	AppendSegment(chunklist, 1, 1, kInitialMapUri);

	auto next_gzip = chunklist->ToGzipData("", false, false, true);
	EXPECT_NE(next_gzip, gzip);
	EXPECT_NE(Gunzip(next_gzip).IndexOf("seg_1_1_video_key_llhls.m4s"), -1);
	EXPECT_EQ(Gunzip(next_gzip), DefaultView(chunklist));
}

namespace
{
	constexpr uint32_t kAlignerVideoTrack = 0;
	constexpr uint32_t kAlignerAudioTrack = 1;
	constexpr uint32_t kAlignerSecondVideoTrack = 2;
	constexpr uint32_t kAlignerSecondAudioTrack = 3;

	// (track id, DTS in ms) of each packet the aligner let through
	using AlignerOutput = std::vector<std::pair<uint32_t, int64_t>>;

	struct AlignerFeeder
	{
		AlignerFeeder(int32_t reference_video_track_id = kAlignerVideoTrack,
					  const std::vector<uint32_t> &video_track_ids = {kAlignerVideoTrack},
					  const std::vector<uint32_t> &audio_track_ids = {kAlignerAudioTrack},
					  size_t max_held_packets = 100)
			: aligner(reference_video_track_id, video_track_ids, audio_track_ids, max_held_packets, "llhls_test")
		{
		}

		void Video(uint32_t track_id, int64_t dts_ms, bool keyframe)
		{
			Push(cmn::MediaType::Video, track_id, dts_ms, keyframe ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag,
				 cmn::BitstreamFormat::H264_AVCC, cmn::PacketType::NALU);
		}

		void Audio(int64_t dts_ms, uint32_t track_id = kAlignerAudioTrack)
		{
			Push(cmn::MediaType::Audio, track_id, dts_ms, MediaPacketFlag::Key,
				 cmn::BitstreamFormat::AAC_RAW, cmn::PacketType::RAW);
		}

		void Push(cmn::MediaType media_type, uint32_t track_id, int64_t dts_ms, MediaPacketFlag flag, cmn::BitstreamFormat bitstream_format, cmn::PacketType packet_type)
		{
			auto data = std::make_shared<ov::Data>();
			uint8_t byte = 0x00;
			data->Append(&byte, 1);

			auto packet = std::make_shared<MediaPacket>(media_type, track_id, data, dts_ms, dts_ms, 0, flag, bitstream_format, packet_type);
			for (const auto &released : aligner.Push(packet, dts_ms * 1000))
			{
				output.emplace_back(released->GetTrackId(), released->GetDts());
			}
		}

		LLHlsKeyframeStartAligner aligner;
		AlignerOutput output;
	};
} // namespace

TEST(LLHlsKeyframeStartAligner, FirstVideoKeyframePassesEverythingInOrder)
{
	AlignerFeeder feeder;

	// Audio ahead of the first video packet is held only until it arrives
	feeder.Audio(0);
	feeder.Audio(21);
	EXPECT_TRUE(feeder.output.empty());
	EXPECT_FALSE(feeder.aligner.IsAligned());

	feeder.Video(kAlignerVideoTrack, 0, true);
	EXPECT_TRUE(feeder.aligner.IsAligned());

	AlignerOutput expected = {{kAlignerAudioTrack, 0}, {kAlignerAudioTrack, 21}, {kAlignerVideoTrack, 0}};
	EXPECT_EQ(feeder.output, expected);
}

TEST(LLHlsKeyframeStartAligner, MidGopJoinStartsAtTheFirstKeyframe)
{
	AlignerFeeder feeder;

	feeder.Video(kAlignerVideoTrack, 100, false);
	feeder.Audio(110);
	feeder.Audio(170);
	EXPECT_TRUE(feeder.output.empty());

	feeder.Video(kAlignerVideoTrack, 166, true);
	EXPECT_TRUE(feeder.aligner.IsAligned());

	// The audio before the keyframe goes with the video before it
	AlignerOutput expected = {{kAlignerAudioTrack, 170}, {kAlignerVideoTrack, 166}};
	EXPECT_EQ(feeder.output, expected);
}

TEST(LLHlsKeyframeStartAligner, AudioArrivingAfterTheKeyframeIsCutByDts)
{
	AlignerFeeder feeder;

	feeder.Video(kAlignerVideoTrack, 100, false);
	feeder.Video(kAlignerVideoTrack, 166, true);

	// A late audio packet may still belong before the start
	EXPECT_FALSE(feeder.aligner.IsAligned());

	feeder.Audio(121);
	feeder.Audio(163);
	EXPECT_FALSE(feeder.aligner.IsAligned());

	feeder.Audio(170);
	EXPECT_TRUE(feeder.aligner.IsAligned());

	AlignerOutput expected = {{kAlignerVideoTrack, 166}, {kAlignerAudioTrack, 170}};
	EXPECT_EQ(feeder.output, expected);
}

TEST(LLHlsKeyframeStartAligner, HeldAudioIsBoundedByTheLimit)
{
	AlignerFeeder feeder(kAlignerVideoTrack, {kAlignerVideoTrack}, {kAlignerAudioTrack}, 2);

	feeder.Audio(0);
	feeder.Audio(21);
	feeder.Audio(42);
	feeder.Video(kAlignerVideoTrack, 0, true);

	// The oldest held packet gave way
	AlignerOutput expected = {{kAlignerAudioTrack, 21}, {kAlignerAudioTrack, 42}, {kAlignerVideoTrack, 0}};
	EXPECT_EQ(feeder.output, expected);
}

TEST(LLHlsKeyframeStartAligner, AudioOnlyStreamIsNotHeld)
{
	AlignerFeeder feeder(-1, {}, {kAlignerAudioTrack});
	EXPECT_TRUE(feeder.aligner.IsAligned());

	feeder.Audio(0);
	EXPECT_EQ(feeder.output, (AlignerOutput{{kAlignerAudioTrack, 0}}));
}

TEST(LLHlsKeyframeStartAligner, EachVideoTrackStartsAtItsOwnKeyframe)
{
	AlignerFeeder feeder(kAlignerVideoTrack, {kAlignerVideoTrack, kAlignerSecondVideoTrack}, {kAlignerAudioTrack});

	feeder.Video(kAlignerVideoTrack, 0, true);
	feeder.Video(kAlignerSecondVideoTrack, 10, false);
	feeder.Audio(21);
	EXPECT_FALSE(feeder.aligner.IsAligned());

	feeder.Video(kAlignerSecondVideoTrack, 40, true);
	EXPECT_TRUE(feeder.aligner.IsAligned());

	// The second track waits for its own keyframe and leaves the audio alone
	AlignerOutput expected = {{kAlignerVideoTrack, 0}, {kAlignerAudioTrack, 21}, {kAlignerSecondVideoTrack, 40}};
	EXPECT_EQ(feeder.output, expected);
}

TEST(LLHlsKeyframeStartAligner, AudioTrackThatPassedIsNotCutAgain)
{
	AlignerFeeder feeder(kAlignerVideoTrack, {kAlignerVideoTrack}, {kAlignerAudioTrack, kAlignerSecondAudioTrack});

	feeder.Video(kAlignerVideoTrack, 100, false);
	feeder.Video(kAlignerVideoTrack, 166, true);
	feeder.Audio(170);

	// The first audio track goes back in DTS while the second has not passed yet
	feeder.Audio(50);
	feeder.Audio(150, kAlignerSecondAudioTrack);
	EXPECT_FALSE(feeder.aligner.IsAligned());

	feeder.Audio(190, kAlignerSecondAudioTrack);
	EXPECT_TRUE(feeder.aligner.IsAligned());

	AlignerOutput expected = {{kAlignerVideoTrack, 166}, {kAlignerAudioTrack, 170}, {kAlignerAudioTrack, 50}, {kAlignerSecondAudioTrack, 190}};
	EXPECT_EQ(feeder.output, expected);
}
