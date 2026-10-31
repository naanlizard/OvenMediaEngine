#include <gtest/gtest.h>
#include <modules/ffmpeg/ffmpeg_codec.h>

#include <vector>

namespace
{
	constexpr int kWidth  = 128;
	constexpr int kHeight = 128;

	std::shared_ptr<MediaFrame> MakeNoiseFrame()
	{
		AVFrame *frame = ::av_frame_alloc();
		frame->format  = AV_PIX_FMT_YUVJ420P;
		frame->width   = kWidth;
		frame->height  = kHeight;
		frame->pts	   = 0;
		if (::av_frame_get_buffer(frame, 0) < 0)
		{
			::av_frame_free(&frame);
			return nullptr;
		}

		uint32_t seed = 1;
		for (int plane = 0; plane < 3; plane++)
		{
			int plane_width	 = (plane == 0) ? kWidth : kWidth / 2;
			int plane_height = (plane == 0) ? kHeight : kHeight / 2;
			for (int y = 0; y < plane_height; y++)
			{
				for (int x = 0; x < plane_width; x++)
				{
					seed = seed * 1664525u + 1013904223u;
					frame->data[plane][y * frame->linesize[plane] + x] = static_cast<uint8_t>(seed >> 24);
				}
			}
		}

		auto media_frame = ffmpeg::compat::ToMediaFrame(cmn::MediaType::Video, frame);
		::av_frame_free(&frame);
		return media_frame;
	}

	// Configures mjpeg with the setters AVCodecImageEncoder::SetParamsJpeg uses.
	std::shared_ptr<MediaPacket> EncodeJpeg(int qscale, const std::shared_ptr<MediaFrame> &media_frame)
	{
		ffmpeg::FFmpegCodec codec;
		if (codec.AllocEncoder(cmn::MediaCodecId::Jpeg) == false)
		{
			return nullptr;
		}

		codec.SetMediaType(cmn::MediaType::Video);
		codec.SetTimeBase(cmn::Timebase(1, 30));
		codec.SetPixelFormat(cmn::VideoPixelFormatId::YUVJ420P);
		codec.SetWidth(kWidth);
		codec.SetHeight(kHeight);
		codec.SetFixedQScale();
		codec.SetGlobalQualityFromQp(qscale);
		codec.SetColorRange(cmn::ColorRange::Full);
		codec.SetStrictCompliance();

		if (codec.Open() == false || codec.SendFrame(media_frame) != ffmpeg::CodecResult::Ok)
		{
			return nullptr;
		}

		auto result = codec.ReceivePacket(cmn::BitstreamFormat::JPEG, cmn::PacketType::RAW);
		return (result.result == ffmpeg::CodecResult::Ok) ? result.packet : nullptr;
	}

	// The 64 entries of the first DQT table (luma); empty when there is none.
	std::vector<uint8_t> LumaDqt(const std::shared_ptr<MediaPacket> &packet)
	{
		const uint8_t *data = packet->GetData()->GetDataAs<uint8_t>();
		size_t length		= packet->GetDataLength();
		for (size_t i = 0; i + 5 + 64 <= length; i++)
		{
			if (data[i] == 0xFF && data[i + 1] == 0xDB)
			{
				return std::vector<uint8_t>(data + i + 5, data + i + 5 + 64);
			}
		}
		return {};
	}
}  // namespace

TEST(FFmpegCodecSendFrame, FixedQScaleSetsMjpegQuantizer)
{
	auto media_frame = MakeNoiseFrame();
	ASSERT_NE(media_frame, nullptr);

	auto q2	 = EncodeJpeg(2, media_frame);
	auto q10 = EncodeJpeg(10, media_frame);
	ASSERT_NE(q2, nullptr);
	ASSERT_NE(q10, nullptr);

	auto dqt2  = LumaDqt(q2);
	auto dqt10 = LumaDqt(q10);
	ASSERT_EQ(dqt2.size(), 64u);
	ASSERT_EQ(dqt10.size(), 64u);
	EXPECT_NE(dqt2, dqt10);
	// mjpeg writes the MPEG-1 intra matrix scaled by qscale; its first AC entry is 16, so 2 * qscale.
	EXPECT_EQ(dqt2[1], 4);
	EXPECT_EQ(dqt10[1], 20);

	EXPECT_LT(q10->GetDataLength(), q2->GetDataLength());
}
