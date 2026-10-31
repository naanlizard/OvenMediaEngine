//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/mediarouter/media_type.h>

#include "bypass_if_match.h"

namespace cfg
{
	namespace vhost
	{
		namespace app
		{
			namespace oprf
			{
				struct ImageProfile : public Item
				{
				protected:
					ov::String _name;
					ov::String _codec;
					ov::String _modules;
					int _width		  = 0;
					int _height		  = 0;
					double _framerate = 0.0;
					BypassIfMatch _bypass_if_match;
					int _skip_frames = -1;
					// fit | crop | stretch
					ov::String _scale_mode = "stretch";
					ov::String _preset;
					int _qscale = 0;
					int _quality = -1;
					int _method = -1;
					bool _lossless = false;
					ov::String _chroma_sampling;
					int _speed = -1;
					int _crf = -1;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(GetName, _name)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetCodec, _codec)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetModules, _modules);
					CFG_DECLARE_CONST_REF_GETTER_OF(GetWidth, _width)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetHeight, _height)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetFramerate, _framerate)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetBypassIfMatch, _bypass_if_match)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetSkipFrames, _skip_frames)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetScaleMode, _scale_mode)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetPreset, _preset)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetQScale, _qscale)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetQuality, _quality)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetMethod, _method)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetLossless, _lossless)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetChromaSampling, _chroma_sampling)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetSpeed, _speed)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetCrf, _crf)

					void SetName(const ov::String &name)
					{
						_name = name;
					}

				protected:
					void MakeList() override
					{
						Register<Optional>("Name", &_name);
						Register<Optional>("Codec", &_codec);
						Register<Optional>("Modules", &_modules);
						Register<Optional>("Width", &_width);
						Register<Optional>("Height", &_height);
						Register<Optional>("Framerate", &_framerate);
						Register<Optional>("SkipFrames", &_skip_frames, nullptr,
										   [=]() -> std::shared_ptr<ConfigError> {
											   if (_framerate > 0 && _skip_frames > 0)
											   {
												   logw("Config", "Use SkipFrames in the settings, the Framerate is ignored.");
											   }

											   return (_skip_frames >= -1 && _skip_frames <= 120) ? nullptr : CreateConfigErrorPtr("SkipFrames must be -1 or between 0 and 120");
										   });
						Register<Optional>("BypassIfMatch", &_bypass_if_match);
						Register<Optional>("ScaleMode", &_scale_mode, nullptr,
										   [=]() -> std::shared_ptr<ConfigError> {
											   return ::cmn::GetVideoScaleModeByName(_scale_mode).has_value() ? nullptr : CreateConfigErrorPtr("ScaleMode must be fit, crop or stretch");
										   });
						Register<Optional>("Preset", &_preset);
						Register<Optional>("QScale", &_qscale);
						Register<Optional>("Quality", &_quality);
						Register<Optional>("Method", &_method);
						Register<Optional>("Lossless", &_lossless);
						Register<Optional>("ChromaSampling", &_chroma_sampling);
						Register<Optional>("Speed", &_speed);
						Register<Optional>("Crf", &_crf);
					}
				};
			}  // namespace oprf
		}  // namespace app
	}  // namespace vhost
}  // namespace cfg