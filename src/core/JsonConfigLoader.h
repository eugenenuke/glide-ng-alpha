#pragma once

#include "IConfigLoader.h"

namespace GlideWrapper {

class JsonConfigLoader : public IConfigLoader {
public:
    bool Load(const std::string& configFilePath, WrapperConfig& outConfig) override;
};

} // namespace GlideWrapper
