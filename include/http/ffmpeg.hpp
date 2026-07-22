#ifndef AUDIOX_HTTP_FFMPEG_HPP
#define AUDIOX_HTTP_FFMPEG_HPP

#include <sys/stat.h>
#include <unistd.h>

namespace {

// Get ffmpeg binary path. Returns path if found and executable, NULL otherwise.
// Automatically chmod's to 0755 if found but not executable.
static inline const char *getFfmpegPath() {
    static const char ffmpeg_path[] = "/audiox/ffmpeg";
    
    // Check if executable
    if (access(ffmpeg_path, X_OK) == 0) {
        return ffmpeg_path;
    }
    
    // Check if exists but not executable, try to chmod it
    if (access(ffmpeg_path, F_OK) == 0) {
        chmod(ffmpeg_path, 0755);
        // Try again
        if (access(ffmpeg_path, X_OK) == 0) {
            return ffmpeg_path;
        }
    }
    
    return NULL;
}

}

#endif // AUDIOX_HTTP_FFMPEG_HPP
