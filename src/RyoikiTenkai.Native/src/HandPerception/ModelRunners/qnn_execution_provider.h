#pragma once

#include <onnxruntime_cxx_api.h>

#include <string>

namespace ryoiki::hand_perception
{
bool tryAppendWindowsMlQnnHtp(
    Ort::Env& environment,
    Ort::SessionOptions& sessionOptions,
    std::string& providerDetail,
    std::string& error);
}
