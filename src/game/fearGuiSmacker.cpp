#include <opent1/game/shell_controls.hpp>
#include <cmath>
#include <algorithm>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgSmackTag = 200172;
inline constexpr Int32 kIdItgLoop = 200173;
inline constexpr Int32 kIdItgPreload = 200174;
inline constexpr Int32 kIdItgPalStart = 200175;
inline constexpr Int32 kIdItgPalColors = 200176;
inline constexpr Int32 kIdItgMovieDoneTag = 200177;
inline constexpr Int32 kIdItgStretch = 200178;
} // namespace

bool SmackerCtrl::attachMovie(std::shared_ptr<const video::Movie> movie) {
    if (audioStarted_ && stopAudio) {
        stopAudio();
    }
    audioStarted_ = false;
    startTime_.reset();
    completionSent_ = false;
    if (!movie) {
        decoder_.reset();
        movieFinished_ = false;
        return false;
    }
    decoder_ = std::make_unique<video::Decoder>(std::move(movie));
    movieFinished_ = false;
    return decoder_->seek(0);
}

bool SmackerCtrl::stepFrame() {
    if (!decoder_ || !decoder_->movie()) {
        return false;
    }
    if (decoder_->next()) {
        return true;
    }
    if (loop_) {
        return decoder_->seek(0);
    }
    movieFinished_ = true;
    return false;
}

std::shared_ptr<const video::Frame> SmackerCtrl::currentFrame() const noexcept {
    return decoder_ ? decoder_->frame() : nullptr;
}

void SmackerCtrl::wake() {
    if (loadMovie) {
        (void)attachMovie(loadMovie(smkTag_));
    } else if (decoder_) {
        if (audioStarted_ && stopAudio) {
            stopAudio();
        }
        audioStarted_ = false;
        (void)decoder_->seek(0);
        movieFinished_ = false;
        completionSent_ = false;
        startTime_.reset();
    }
}

void SmackerCtrl::close() {
    if (audioStarted_ && stopAudio) {
        stopAudio();
    }
    audioStarted_ = false;
    decoder_.reset();
    startTime_.reset();
}

void SmackerCtrl::complete() {
    if (completionSent_) {
        return;
    }
    movieFinished_ = true;
    completionSent_ = true;
    const auto notify = [this, life = lifetime()] {
        if (life.expired()) {
            return;
        }
        if (audioStarted_ && stopAudio) {
            stopAudio();
        }
        audioStarted_ = false;
        if (movieDoneTag_) {
            (void)gui::Control::message(*this, UInt32(movieDoneTag_));
        } else {
            Parent::action();
        }
    };
    if (canvas() && canvas()->dispatching()) {
        canvas()->afterDispatch(notify);
    } else {
        notify();
    }
}

void SmackerCtrl::tick(double seconds) {
    if (!decoder_ || movieFinished_ || !std::isfinite(seconds)) {
        return;
    }
    if (!startTime_) {
        startTime_ = seconds;
    }
    if (seconds < *startTime_) {
        return;
    }
    const auto movie = decoder_->movie();
    if (!audioStarted_ && startAudio) {
        audioStarted_ = true;
        const auto start = [this, life = lifetime(), movie, loop = loop_] {
            if (!life.expired() && decoder_ && decoder_->movie() == movie && !movieFinished_ &&
                startAudio) {
                startAudio(movie, loop);
            }
        };
        if (canvas() && canvas()->dispatching()) {
            canvas()->afterDispatch(start);
        } else {
            start();
        }
    }
    const auto elapsed = (seconds - *startTime_) * 1000000;
    if (!loop_ && elapsed >= double(movie->durationUs())) {
        complete();
        return;
    }
    const auto time = loop_ ? std::fmod(elapsed, double(movie->durationUs())) : elapsed;
    const auto frame = UInt32(time / double(movie->frameTimeUs()));
    if (!decoder_->frame() || decoder_->frame()->index != frame) {
        if (!decoder_->seek(frame)) {
            complete();
        }
    }
}

bool SmackerCtrl::event(const gui::Event& event) {
    if (event.type == gui::EventType::KeyDown || event.type == gui::EventType::Down) {
        complete();
        return true;
    }
    return Parent::event(event);
}

bool SmackerCtrl::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const auto frame = currentFrame();
    if (!frame) {
        return drawChildren(surface, offset, clip);
    }
    if (frame->bitmap.palette) {
        auto palette = surface.palette() ? *surface.palette() : *frame->bitmap.palette;
        const auto start = std::clamp(palStart_, 0, 256),
                   count = std::clamp(palColors_, 0, 256 - start);
        std::copy_n(frame->bitmap.palette->palettes[0].colors.begin() + start,
                    count,
                    palette.palettes[0].colors.begin() + start);
        if (!surface.setPalette(palette)) {
            return false;
        }
        if (applyPalette) {
            applyPalette(palette);
        }
    }
    const auto saved = surface.clip();
    surface.setClip(clip);
    bool drawn{};
    if (!stretch_) {
        drawn = surface.blit(frame->bitmap, offset);
    } else {
        auto bitmap = gfx::Bitmap::create(fields_.extent.x, fields_.extent.y);
        if (!bitmap) {
            surface.setClip(saved);
            return false;
        }
        for (Int32 y = 0; y < bitmap->height; ++y) {
            const auto row =
                frame->bitmap.row(Int32(Int64(y) * frame->bitmap.height / bitmap->height));
            for (Int32 x = 0; x < bitmap->width; ++x) {
                bitmap->row(y)[std::size_t(x)] =
                    row[std::size_t(Int64(x) * frame->bitmap.width / bitmap->width)];
            }
        }
        drawn = surface.blit(*bitmap, offset);
    }
    surface.setClip(saved);
    return drawn && drawChildren(surface, offset, clip);
}

std::function<void()> SmackerCtrl::checkpoint() {
    auto base = Parent::checkpoint();
    const auto st = smkTag_;
    const auto lp = loop_;
    const auto md = movieDoneTag_;
    const auto pr = preload_;
    const auto ps = palStart_;
    const auto pc = palColors_;
    const auto sr = stretch_;
    const auto decoder = decoder_ ? std::make_shared<video::Decoder>(*decoder_) : nullptr;
    const auto start = startTime_;
    const auto finished = movieFinished_;
    const auto sent = completionSent_;
    const auto audio = audioStarted_;
    return [this,
            base = std::move(base),
            st,
            lp,
            md,
            pr,
            ps,
            pc,
            sr,
            decoder,
            start,
            finished,
            sent,
            audio] {
        base();
        smkTag_ = st;
        loop_ = lp;
        movieDoneTag_ = md;
        preload_ = pr;
        palStart_ = ps;
        palColors_ = pc;
        stretch_ = sr;
        decoder_ = decoder ? std::make_unique<video::Decoder>(*decoder) : nullptr;
        startTime_ = start;
        movieFinished_ = finished;
        completionSent_ = sent;
        audioStarted_ = audio;
    };
}

bool SmackerCtrl::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgSmackTag, sim::Inspect::Integer{smkTag_});
    inspector.write(kIdItgLoop, loop_);
    inspector.write(kIdItgMovieDoneTag, sim::Inspect::Integer{movieDoneTag_});
    inspector.write(kIdItgPreload, preload_);
    inspector.write(kIdItgPalStart, sim::Inspect::Integer{palStart_});
    inspector.write(kIdItgPalColors, sim::Inspect::Integer{palColors_});
    inspector.write(kIdItgStretch, stretch_);
    return true;
}

bool SmackerCtrl::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgSmackTag, v)) {
        return false;
    }
    smkTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgLoop, v)) {
        return false;
    }
    loop_ = std::get<bool>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgMovieDoneTag, v)) {
        return false;
    }
    movieDoneTag_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgPreload, v)) {
        return false;
    }
    preload_ = std::get<bool>(v);
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgPalStart, v)) {
        return false;
    }
    palStart_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Int32, kIdItgPalColors, v)) {
        return false;
    }
    palColors_ = std::get<sim::Inspect::Integer>(v).value;
    if (!inspector.read(sim::Inspect::Type::Bool, kIdItgStretch, v)) {
        return false;
    }
    stretch_ = std::get<bool>(v);
    return true;
}

Persistent::Error
SmackerCtrl::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 smk{};
    bool lp{};
    Int32 doneTag = 0;
    bool pre{};
    Int32 pStart{};
    Int32 pCols{};
    bool str{};
    if (!sio.read(smk) || !sio.read(lp) || !sio.read(doneTag) || !sio.read(pre) ||
        !sio.read(pStart) || !sio.read(pCols) || !sio.read(str)) {
        return Persistent::Error::ReadError;
    }
    if (pStart < 0 || pStart > 256 || pCols < 0 || pCols > 256) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    smkTag_ = smk;
    loop_ = lp;
    movieDoneTag_ = doneTag;
    preload_ = pre;
    palStart_ = pStart;
    palColors_ = pCols;
    stretch_ = str;
    return Persistent::Error::Ok;
}

Persistent::Error
SmackerCtrl::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (palStart_ < 0 || palStart_ > 256 || palColors_ < 0 || palColors_ > 256) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(smkTag_) || !sio.write(loop_) || !sio.write(movieDoneTag_) ||
        !sio.write(preload_) || !sio.write(palStart_) || !sio.write(palColors_) ||
        !sio.write(stretch_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
