#include <gtest/gtest.h>

#include <base/info/application.h>
#include <base/info/media_track.h>
#include <base/mediarouter/media_buffer.h>

#include "thumbnail_application.h"
#include "thumbnail_stream.h"

namespace
{
	class TestApplicationInfo : public info::Application
	{
	public:
		using info::Application::GetInvalidApplication;
	};

	std::shared_ptr<MediaTrack> MakeTrack(uint32_t id, cmn::MediaType media_type, cmn::MediaCodecId codec_id)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(id);
		track->SetMediaType(media_type);
		track->SetCodecId(codec_id);
		return track;
	}

	std::shared_ptr<MediaPacket> MakePacket(cmn::MediaType media_type, uint32_t track_id, cmn::BitstreamFormat format)
	{
		auto data = std::make_shared<ov::Data>();
		uint8_t byte = 0x00;
		data->Append(&byte, 1);

		return std::make_shared<MediaPacket>(media_type, track_id, data, 0, 0, 0, MediaPacketFlag::Key, format, cmn::PacketType::RAW);
	}
}  // namespace

TEST(ThumbnailStream, DropsOnlyPacketsOfItsNonImageTracks)
{
	auto application = std::make_shared<ThumbnailApplication>(nullptr, TestApplicationInfo::GetInvalidApplication());

	info::Stream info(*application, StreamSourceType::Ovt);
	info.AddTrack(MakeTrack(0, cmn::MediaType::Video, cmn::MediaCodecId::H264));
	info.AddTrack(MakeTrack(1, cmn::MediaType::Audio, cmn::MediaCodecId::Aac));
	info.AddTrack(MakeTrack(2, cmn::MediaType::Video, cmn::MediaCodecId::Jpeg));
	info.AddTrack(MakeTrack(3, cmn::MediaType::Data, cmn::MediaCodecId::None));
	auto stream = ThumbnailStream::Create(application, info);

	auto packet = [&](cmn::MediaType media_type, uint32_t track_id, cmn::BitstreamFormat format) {
		auto media_packet = MakePacket(media_type, track_id, format);
		media_packet->SetTrack(stream->GetTrack(track_id));
		return media_packet;
	};

	EXPECT_TRUE(stream->CanDropWithoutQueueing(packet(cmn::MediaType::Video, 0, cmn::BitstreamFormat::H264_ANNEXB)));
	EXPECT_TRUE(stream->CanDropWithoutQueueing(packet(cmn::MediaType::Audio, 1, cmn::BitstreamFormat::AAC_ADTS)));
	EXPECT_TRUE(stream->CanDropWithoutQueueing(MakePacket(cmn::MediaType::Video, 0, cmn::BitstreamFormat::H264_ANNEXB)));

	EXPECT_FALSE(stream->CanDropWithoutQueueing(packet(cmn::MediaType::Video, 2, cmn::BitstreamFormat::JPEG)));
	EXPECT_FALSE(stream->CanDropWithoutQueueing(packet(cmn::MediaType::Data, 3, cmn::BitstreamFormat::OVEN_EVENT)));

	// A new version of a track still goes to the worker, which applies it to the stream (e.g. an image track
	// that became H.264, so its last image is no longer served)
	auto changed = MakePacket(cmn::MediaType::Video, 0, cmn::BitstreamFormat::H264_ANNEXB);
	changed->SetTrack(MakeTrack(0, cmn::MediaType::Video, cmn::MediaCodecId::H264));
	EXPECT_FALSE(stream->CanDropWithoutQueueing(changed));
}

TEST(ThumbnailStream, DropsEveryAudioVideoPacketOfAStreamWithoutImages)
{
	auto application = std::make_shared<ThumbnailApplication>(nullptr, TestApplicationInfo::GetInvalidApplication());

	info::Stream info(*application, StreamSourceType::Ovt);
	info.AddTrack(MakeTrack(0, cmn::MediaType::Video, cmn::MediaCodecId::H264));
	info.AddTrack(MakeTrack(1, cmn::MediaType::Data, cmn::MediaCodecId::None));
	auto stream = ThumbnailStream::Create(application, info);

	// Start fails without an image track and the stream is left in ERROR, which the worker skips
	stream->SetState(pub::Stream::State::ERROR);

	auto changed = MakePacket(cmn::MediaType::Video, 0, cmn::BitstreamFormat::H264_ANNEXB);
	changed->SetTrack(MakeTrack(0, cmn::MediaType::Video, cmn::MediaCodecId::H264));
	EXPECT_TRUE(stream->CanDropWithoutQueueing(changed));

	EXPECT_FALSE(stream->CanDropWithoutQueueing(MakePacket(cmn::MediaType::Data, 1, cmn::BitstreamFormat::OVEN_EVENT)));
}
