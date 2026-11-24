#ifndef NBT_VIDEO_ACKS_HPP
#define NBT_VIDEO_ACKS_HPP

#include <deque>
#include <pthread.h>
#include <stdint.h>

namespace nbt {

// Tokens identify individual frames so an out-of-order drop or a late FIFO handoff cannot release another frame's ACK.
class VideoAcks {
public:
    VideoAcks() : enabled_(false), gate_(false), fps_(0), next_(0), sequence_(0) {
        pthread_mutex_init(&mutex_, 0);
    }
    ~VideoAcks() { pthread_mutex_destroy(&mutex_); }

    void start(bool enabled, bool gate, unsigned fps) {
        pthread_mutex_lock(&mutex_);
        enabled_ = enabled;
        gate_ = gate;
        fps_ = fps;
        next_ = 0;
        pending_.clear();
        pthread_mutex_unlock(&mutex_);
    }

    uint64_t frame(double now, bool& overflow) {
        pthread_mutex_lock(&mutex_);
        overflow = false;
        uint64_t token = 0;
        if (enabled_ && (gate_ || fps_)) {
            if (pending_.size() == 64) {
                pending_.pop_front();
                overflow = true;
            }
            token = ++sequence_;
            if (!token) token = ++sequence_;
            pending_.push_back(Frame(token, now, !gate_));
        }
        pthread_mutex_unlock(&mutex_);
        return token;
    }

    bool consumed(uint64_t token) {
        if (!token) return false;
        pthread_mutex_lock(&mutex_);
        bool found = false;
        for (std::deque<Frame>::iterator i = pending_.begin(); i != pending_.end(); ++i) {
            if (i->token == token) {
                i->ready = true;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&mutex_);
        return found;
    }

    bool take(double now, bool& forced) {
        pthread_mutex_lock(&mutex_);
        forced = false;
        bool result = false;
        if (!pending_.empty()) {
            const Frame& frame = pending_.front();
            forced = !frame.ready && now - frame.arrived >= 15.0;
            if (forced || (frame.ready && (!fps_ || now >= next_))) {
                pending_.pop_front();
                if (!forced && fps_) next_ = now + 1.0 / fps_;
                result = true;
            }
        }
        pthread_mutex_unlock(&mutex_);
        return result;
    }

    unsigned tickMs(double now) {
        pthread_mutex_lock(&mutex_);
        unsigned tick = 1000;
        if (!pending_.empty()) {
            const Frame& frame = pending_.front();
            const double due = frame.ready ? (fps_ ? next_ : now) : frame.arrived + 15.0;
            const double wait = due - now;
            if (wait <= 0) tick = 1;
            else if (wait < 1.0) tick = (unsigned)(wait * 1000.0) + 1;
        }
        pthread_mutex_unlock(&mutex_);
        return tick;
    }

    unsigned resetStream() {
        pthread_mutex_lock(&mutex_);
        const unsigned count = (unsigned)pending_.size();
        pending_.clear();
        next_ = 0;
        pthread_mutex_unlock(&mutex_);
        return count;
    }

private:
    struct Frame {
        uint64_t token;
        double arrived;
        bool ready;
        Frame(uint64_t t, double a, bool r) : token(t), arrived(a), ready(r) {}
    };
    VideoAcks(const VideoAcks&);
    VideoAcks& operator=(const VideoAcks&);
    pthread_mutex_t mutex_;
    bool enabled_, gate_;
    unsigned fps_;
    double next_;
    uint64_t sequence_;
    std::deque<Frame> pending_;
};

} // namespace nbt

#endif
