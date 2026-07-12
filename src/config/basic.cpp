#include "config/context.hpp"

ConfigStore::ConfigStore(Audiox *context) : app(context) {}

int ConfigStore::getSampleRate() const {
    return 0;
}

void ConfigStore::setSampleRate(int rate) {
    
}

RouterConfig ConfigStore::router() const {
    return RouterConfig();
}
