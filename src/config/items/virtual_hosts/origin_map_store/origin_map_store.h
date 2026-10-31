//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <charconv>

#include "redis_server.h"

namespace cfg
{
	namespace vhost
	{
		namespace orgn
		{
			struct OriginMapStore : public Item
			{
				CFG_DECLARE_CONST_REF_GETTER_OF(GetRedisServer, _redis_server)
				CFG_DECLARE_CONST_REF_GETTER_OF(GetOriginHostName, _origin_host_name)
				CFG_DECLARE_CONST_REF_GETTER_OF(GetOriginPort, _origin_port)

			protected:
				void MakeList() override
				{
					Register("RedisServer", &_redis_server);
					Register<Optional>("OriginHostName", &_origin_host_name);
					// Parsed here because an int item accepts a numeric prefix such as "3.1e4" as 3
					Register<Optional>("OriginPort", &_origin_port_text, nullptr,
						[=]() -> std::shared_ptr<ConfigError> {
							auto text = _origin_port_text.Trim();
							int port = 0;
							auto [end, error] = std::from_chars(text.CStr(), text.CStr() + text.GetLength(), port);
							if (error != std::errc() || end != text.CStr() + text.GetLength() || port < 1 || port > 65535)
							{
								return CreateConfigErrorPtr("OriginPort must be an integer between 1 and 65535: %s", _origin_port_text.CStr());
							}
							_origin_port = port;
							return nullptr;
						});
				}
				
				RedisServer _redis_server;
				ov::String _origin_host_name;
				ov::String _origin_port_text;
				// 0 when not configured
				int _origin_port = 0;
			};
		}  // namespace orgn
	}	   // namespace vhost
}  // namespace cfg