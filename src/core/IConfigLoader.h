#pragma once

#include "WrapperConfig.h"
#include <string>

namespace GlideWrapper {

class IConfigLoader {
public:
    virtual ~IConfigLoader() = default;
    
    virtual bool Load(const std::string& configFilePath, WrapperConfig& outConfig) = 0;
};

} // namespace GlideWrapper
