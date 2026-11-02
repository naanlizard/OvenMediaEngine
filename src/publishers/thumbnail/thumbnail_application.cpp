#include "thumbnail_application.h"

#include "thumbnail_private.h"
#include "thumbnail_stream.h"

std::shared_ptr<ThumbnailApplication> ThumbnailApplication::Create(const std::shared_ptr<pub::Publisher> &publisher, const info::Application &application_info)
{
	auto application = std::make_shared<ThumbnailApplication>(publisher, application_info);
	application->Start();
	return application;
}

ThumbnailApplication::ThumbnailApplication(const std::shared_ptr<pub::Publisher> &publisher, const info::Application &application_info)
	: Application(publisher, application_info)
{
	auto thumbnail_config = application_info.GetConfig().GetPublishers().GetThumbnailPublisher();

	bool is_parsed;
	const auto &cross_domains = thumbnail_config.GetCrossDomains(&is_parsed);

	if (is_parsed)
	{
		_cors_manager.SetCrossDomains(application_info.GetVHostAppName(), cross_domains);
	}
}

ThumbnailApplication::~ThumbnailApplication()
{
	Stop();
	logtt("ThumbnailApplication(%d) has been terminated finally", GetId());
}

bool ThumbnailApplication::Start()
{
	return Application::Start();
}

bool ThumbnailApplication::Stop()
{
	return Application::Stop();
}

bool ThumbnailApplication::OnSendFrame(const std::shared_ptr<info::Stream> &stream, const std::shared_ptr<MediaPacket> &media_packet)
{
	auto pub_stream = GetStream(stream->GetId());

	// Every stream of this application is a ThumbnailStream (CreateStream)
	auto thumbnail_stream = static_cast<ThumbnailStream *>(pub_stream.get());
	if (thumbnail_stream != nullptr && thumbnail_stream->CanDropWithoutQueueing(media_packet))
	{
		return true;
	}

	return EnqueueMediaPacket(stream->GetId(), pub_stream, media_packet);
}

std::shared_ptr<pub::Stream> ThumbnailApplication::CreateStream(const std::shared_ptr<info::Stream> &info, uint32_t worker_count)
{
	logtt("Created stream : %s/%u", info->GetName().CStr(), info->GetId());

	return ThumbnailStream::Create(GetSharedPtrAs<pub::Application>(), *info);
}

bool ThumbnailApplication::DeleteStream(const std::shared_ptr<info::Stream> &info)
{
	logtt("ThumbnailApplication::DeleteStream : %s/%u", info->GetName().CStr(), info->GetId());

	auto stream = std::static_pointer_cast<ThumbnailStream>(GetStream(info->GetId()));
	if (stream == nullptr)
	{
		logte("ThumbnailApplication::Delete stream failed. Cannot find stream (%s)", info->GetName().CStr());
		return false;
	}

	logtt("ThumbnailApplication %s/%s stream has been deleted", GetVHostAppName().CStr(), stream->GetName().CStr());

	return true;
}
