#include <opent1/game/commander_huds.hpp>

namespace opent1::game {

FGHCommandObjectivePage::FGHCommandObjectivePage() {
    cellSize = {480, 18};
}

void FGHCommandObjectivePage::setObjectives(std::vector<std::string> objectives) {
    if (objectives.size() > MaxObjectives) {
        objectives.resize(MaxObjectives);
    }
    if (objectives == objectives_) {
        return;
    }
    objectives_ = std::move(objectives);
    formattedWidth_ = 0;
    setSize({1, static_cast<Int32>(objectives_.size())});
}

void FGHCommandObjectivePage::tick(double) {
    const auto width = parent() ? parent()->fields().extent.x : fields_.extent.x;
    if (width <= 0 || (width == formattedWidth_ && formatted_.size() == objectives_.size())) {
        return;
    }
    std::vector<std::unique_ptr<gui::TextFormat>> next;
    Int32 height = 4;
    for (const auto& text : objectives_) {
        auto formatted = std::make_unique<gui::TextFormat>();
        formatted->fonts = objectiveFonts;
        formatted->bitmap = objectiveBitmap;
        formatted->variable = objectiveVariable;
        if (!formatted->format(text, width)) {
            return;
        }
        height += formatted->height();
        next.push_back(std::move(formatted));
    }
    if (!resize(fields_.position, {width, height})) {
        return;
    }
    formatted_ = std::move(next);
    formattedWidth_ = width;
}

bool FGHCommandObjectivePage::draw(gfx::SoftwareSurface& surface,
                                   Point2I offset,
                                   RectI clip) const {
    for (const auto& formatted : formatted_) {
        if (!formatted->draw(surface, offset, clip)) {
            return false;
        }
        offset.y += formatted->height();
    }
    return true;
}

std::string FGHCommandObjectivePage::getCellText(Point2I cell) const {
    if (cell.y < 0 || static_cast<std::size_t>(cell.y) >= objectives_.size()) {
        return {};
    }
    return objectives_[static_cast<std::size_t>(cell.y)];
}

} // namespace opent1::game
