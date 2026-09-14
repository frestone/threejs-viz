#include "viz/config.h"

#include <gtest/gtest.h>

namespace {

TEST(ImageChannelConfigTest, Uses480By270ThumbnailSizeByDefault) {
    viz::SceneConfig config;

    viz::SceneConfig::loadImageChannelsFromJson(config, R"json(
        {
            "imageChannels": [
                {
                    "id": "camera_front",
                    "topic": "/camera/front",
                    "codec": "hevc"
                }
            ]
        }
    )json");

    ASSERT_EQ(config.imageChannels.size(), 1u);
    EXPECT_EQ(config.imageChannels[0].thumbnailWidth, 480);
    EXPECT_EQ(config.imageChannels[0].thumbnailHeight, 270);
}

TEST(ImageChannelConfigTest, ReadsPerChannelThumbnailSize) {
    viz::SceneConfig config;

    viz::SceneConfig::loadImageChannelsFromJson(config, R"json(
        {
            "imageChannels": [
                {
                    "id": "camera_front",
                    "topic": "/camera/front",
                    "thumbnailWidth": 640,
                    "thumbnailHeight": 360
                }
            ]
        }
    )json");

    ASSERT_EQ(config.imageChannels.size(), 1u);
    EXPECT_EQ(config.imageChannels[0].thumbnailWidth, 640);
    EXPECT_EQ(config.imageChannels[0].thumbnailHeight, 360);
}

}  // namespace