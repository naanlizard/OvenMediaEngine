#include <config/config.h>
#include <gtest/gtest.h>
#include <stdlib.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
	template <typename Titem>
	bool ParseItem(const char *name, const std::string &body, Titem *item)
	{
		char path[]	 = "/tmp/ome_rendition_score_test_XXXXXX";
		const int fd = ::mkstemp(path);
		if (fd < 0)
		{
			return false;
		}
		::close(fd);

		{
			std::ofstream out(path);
			out << "<" << name << ">" << body << "</" << name << ">";
		}

		bool parsed = false;

		try
		{
			cfg::DataSource data_source(cfg::DataType::Xml, path, cfg::ItemName(name));
			item->FromDataSource(name, cfg::ItemName(name), data_source);
			parsed = true;
		}
		catch (const cfg::ConfigError &)
		{
			parsed = false;
		}

		::remove(path);

		return parsed;
	}

	bool ParseRendition(const std::string &score_element, cfg::vhost::app::oprf::Rendition *rendition)
	{
		return ParseItem("Rendition", "<Name>audio</Name><Audio>bypass_audio</Audio>" + score_element, rendition);
	}

	bool ParseRenditionTemplate(const std::string &score_element, cfg::vhost::app::oprf::RenditionTemplate *rendition_template)
	{
		return ParseItem("RenditionTemplate", "<Name>template</Name>" + score_element, rendition_template);
	}
}  // namespace

TEST(RenditionScore, AbsentScoreStaysUnset)
{
	cfg::vhost::app::oprf::Rendition rendition;
	ASSERT_TRUE(ParseRendition("", &rendition));
	EXPECT_DOUBLE_EQ(rendition.GetScore(), -1.0);

	cfg::vhost::app::oprf::RenditionTemplate rendition_template;
	ASSERT_TRUE(ParseRenditionTemplate("", &rendition_template));
	EXPECT_DOUBLE_EQ(rendition_template.GetScore(), -1.0);
}

TEST(RenditionScore, KeepsPositiveScore)
{
	for (const auto &[element, expected] : std::vector<std::pair<std::string, double>>{
			 {"<Score>100</Score>", 100.0},
			 {"<Score>1</Score>", 1.0},
			 {"<Score>0.001</Score>", 0.001},
			 {"<Score>0.0009</Score>", 0.0009},
			 {"<Score>0.0005</Score>", 0.0005},
		 })
	{
		cfg::vhost::app::oprf::Rendition rendition;
		ASSERT_TRUE(ParseRendition(element, &rendition)) << element;
		EXPECT_DOUBLE_EQ(rendition.GetScore(), expected) << element;

		cfg::vhost::app::oprf::RenditionTemplate rendition_template;
		ASSERT_TRUE(ParseRenditionTemplate(element, &rendition_template)) << element;
		EXPECT_DOUBLE_EQ(rendition_template.GetScore(), expected) << element;
	}
}

TEST(RenditionScore, RejectsScoreBelowMinimum)
{
	// Below 0.0005 a score prints as SCORE=0.000, and the config layer turns an empty element or one
	// with no leading number into `0`
	for (const char *element : {"<Score>0</Score>", "<Score>-1</Score>", "<Score>0.0004</Score>", "<Score>0.00049</Score>",
								"<Score></Score>", "<Score>high</Score>", "<Score>nan</Score>", "<Score>inf</Score>"})
	{
		cfg::vhost::app::oprf::Rendition rendition;
		EXPECT_FALSE(ParseRendition(element, &rendition)) << element;

		cfg::vhost::app::oprf::RenditionTemplate rendition_template;
		EXPECT_FALSE(ParseRenditionTemplate(element, &rendition_template)) << element;
	}
}

TEST(RenditionScore, RelayedScoreIsCheckedLikeTheConfiguredOne)
{
	for (double score : {100.0, 0.001, 0.0005})
	{
		EXPECT_TRUE(info::Rendition::IsValidScore(score)) << score;
	}

	for (double score : {0.0, -1.0, 0.0004, std::nan(""), HUGE_VAL})
	{
		EXPECT_FALSE(info::Rendition::IsValidScore(score)) << score;
	}
}
