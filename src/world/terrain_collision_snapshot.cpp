#include <opent1/world/terrain_collision_snapshot.hpp>
#include <ranges>
#include "collision_geometry.hpp"
#include <opent1/math/plane.hpp>
#include <bit>
#include <map>

namespace opent1::world {
namespace {
// Port of grdHeight.cpp and grdCollision.cpp. Keep the four crossing streams,
// strict above test, hole toggles, endpoint path and discontinuous block order.
struct NativeBlock {
    const TerrainGridBlock* block{};
    Int32 id{};

    const Flt32* getHeightMap() const {
        return block->heights.data();
    }

    const TerrainGridMaterial* getMaterialMap() const {
        return block->materials.data();
    }

    int getHeightMapWidth() const {
        return int(block->width + 1);
    }

    const Flt32* getHeight(int, Point2I p) const {
        if (p.x < 0 || p.y < 0 || p.x >= int(block->width) || p.y >= int(block->width)) {
            throw std::out_of_range("native terrain triangle outside block");
        }
        return block->heights.data() + p.y * (block->width + 1) + p.x;
    }

    Int32 getId() const {
        return id;
    }
};

struct NativeFile {
    const TerrainGridResources& resources;
    std::map<Int32, NativeBlock> blocks;

    explicit NativeFile(const TerrainGridResources& r) : resources(r) {
        for (auto i : std::views::iota(std::size_t{0}, r.blocks.size())) {
            blocks.emplace(r.grid->blocks[i].id,
                           NativeBlock{r.blocks[i].get(), r.grid->blocks[i].id});
        }
    }

    int getScale() const {
        return resources.grid->scale;
    }

    int shift() const {
        return resources.grid->detailCount - 1;
    }

    Point2I getSize() const {
        return resources.grid->size;
    }

    unsigned materialCount() const {
        return resources.materials.list().materialsPerDetail > 0
                   ? unsigned(resources.materials.list().materialsPerDetail)
                   : 256U;
    }

    const NativeBlock* getBlock(Point2I p) const {
        const auto id = resources.grid->blockMap[std::size_t(p.y) * resources.grid->size.x + p.x];
        if (id == -1) {
            return nullptr;
        }
        const auto& b = blocks.at(id);
        if (!b.block) {
            throw std::out_of_range("terrain collision outside resident working set");
        }
        return &b;
    }
};

struct GridHeight {
    enum SplitType { Split45, Split135 };

    enum FaceID { FaceA, FaceB };

    float width{};
    SplitType split{};
    const Flt32* heights{};
    int dx{}, dy{};

    FaceID getFace45(Point2F p) const {
        return p.x > p.y ? FaceB : FaceA;
    }

    FaceID getFace135(Point2F p) const {
        return width - p.x > p.y ? FaceA : FaceB;
    }

    void getNormal(const Point2F&, Point3F*);
    void getNormal45a(Point3F*);
    void getNormal45b(Point3F*);
    void getNormal135a(Point3F*);
    void getNormal135b(Point3F*);
    float getHeight(const Point2F&);
    float getHeight45a(const Point2F&);
    float getHeight45b(const Point2F&);
    float getHeight135a(const Point2F&);
    float getHeight135b(const Point2F&);
    bool intersect(const LineSeg3F&, Point3F*);
};

struct GridCollision {
    const NativeFile* terrainFile{};
    const TMat3F* transform{};
    const Flt32* heightField{};
    const TerrainGridMaterial* materialMap{};
    bool tube{}, hitHoles{}, justLOS{};
    int blockShift{}, grdSize{};
    float radius{};
    CollisionSurface surface;
    std::size_t work = collision_detail::MaxWork;
    TerrainGridMaterial fallback;
    float GetHeight(int, int);
    Point3F GetPoint(int, int);
    const TerrainGridMaterial* GetMaterial(int, int);
    bool collide(const Point3F&, const Point3F&, bool, bool);
    bool collide(const Point3F&, const Point3F&, float);
    bool collideFile(const Point3F&, const Point3F&);
    bool collideBlock(const NativeBlock*, const Point3F&, const Point3F&);
};

constexpr float WalkerInfinity = 1e20f;

void GridHeight::getNormal(const Point2F& off, Point3F* normal) {
    switch (split) {
        case Split45:
            (getFace45(off) == FaceA) ? getNormal45a(normal) : getNormal45b(normal);
            break;
        case Split135:
            (getFace135(off) == FaceA) ? getNormal135a(normal) : getNormal135b(normal);
            break;
    }
}

void GridHeight::getNormal45a(Point3F* v1) {
    v1->x = heights[dy] - heights[dx + dy];
    v1->y = heights[0] - heights[dy];
    v1->z = width;
}

void GridHeight::getNormal45b(Point3F* v2) {
    v2->x = heights[0] - heights[dx];
    v2->y = heights[dx] - heights[dx + dy];
    v2->z = width;
}

void GridHeight::getNormal135a(Point3F* v1) {
    v1->x = heights[0] - heights[dx];
    v1->y = heights[0] - heights[dy];
    v1->z = width;
}

void GridHeight::getNormal135b(Point3F* v2) {
    v2->x = heights[dy] - heights[dx + dy];
    v2->y = heights[dx] - heights[dx + dy];
    v2->z = width;
}

float GridHeight::getHeight(const Point2F& off) {
    float height{};
    switch (split) {
        case Split45:
            height = (getFace45(off) == FaceA) ? getHeight45a(off) : getHeight45b(off);
            break;
        case Split135:
            height = (getFace135(off) == FaceA) ? getHeight135a(off) : getHeight135b(off);
            break;
    }
    return height;
}

float GridHeight::getHeight45a(const Point2F& off) {
    return heights[dy] + ((heights[dx + dy] - heights[dy]) * off.x +
                          (heights[0] - heights[dy]) * (width - off.y)) /
                             width;
}

float GridHeight::getHeight45b(const Point2F& off) {
    return heights[dx] + ((heights[0] - heights[dx]) * (width - off.x) +
                          (heights[dx + dy] - heights[dx]) * off.y) /
                             width;
}

float GridHeight::getHeight135a(const Point2F& off) {
    return heights[0] +
           ((heights[dx] - heights[0]) * off.x + (heights[dy] - heights[0]) * off.y) / width;
}

float GridHeight::getHeight135b(const Point2F& off) {
    return heights[dx + dy] + ((heights[dy] - heights[dx + dy]) * (width - off.x) +
                               (heights[dx] - heights[dx + dy]) * (width - off.y)) /
                                  width;
}

constexpr float HeightEpsilon = .0001f;

bool GridHeight::intersect(const LineSeg3F& line, Point3F* ip) {
    PlaneF plane;
    Point3F normal;
    switch (split) {
        case Split45: {
            getNormal45a(&normal);
            plane.set(Point3F(0.0f, 0.0f, heights[0]), normal);
            if (plane.intersect(line, *ip) && ip->x > -HeightEpsilon &&
                ip->y < width + HeightEpsilon && getFace45({ip->x, ip->y}) == FaceA)
                return true;
            getNormal45b(&normal);
            plane.set(Point3F(0.0f, 0.0f, heights[0]), normal);
            if (plane.intersect(line, *ip) && getFace45({ip->x, ip->y}) == FaceB)
                return true;
            return false;
        }
        case Split135: {
            getNormal135a(&normal);
            plane.set(Point3F(0.0f, width, heights[dy]), normal);
            if (plane.intersect(line, *ip) && ip->x > -HeightEpsilon && ip->y > -HeightEpsilon &&
                getFace135({ip->x, ip->y}) == FaceA)
                return true;
            getNormal135b(&normal);
            plane.set(Point3F(0.0f, width, heights[dy]), normal);
            if (plane.intersect(line, *ip) && getFace135({ip->x, ip->y}) == FaceB)
                return true;
            return false;
        }
    }
    return false;
}

inline float GridCollision::GetHeight(int x, int y) {
    int bs = 1 << blockShift;
    if (x < 0)
        x = 0;
    else if (x > bs)
        x = bs;
    if (y < 0)
        y = 0;
    else if (y > bs)
        y = bs;

    return *(heightField + x + y + (y << blockShift));
}

const TerrainGridMaterial* GridCollision::GetMaterial(int x, int y) {
    const int n = 1 << blockShift;
    x = std::clamp(x, 0, n - 1);
    y = std::clamp(y, 0, n - 1);
    const auto* material = materialMap + x + y * n;
    if (material->index >= terrainFile->materialCount()) {
        // Original release code repairs an invalid index in place. Keep the
        // same lookup without mutating an immutable resource snapshot.
        fallback = *material;
        fallback.index = 0;
        return &fallback;
    }
    return material;
}

inline Point3F GridCollision::GetPoint(int x, int y) {
    return Point3F(float(x * (1 << grdSize)), float(y * (1 << grdSize)), GetHeight(x, y));
}

inline void CalcTs(float& curT, float& deltaT, float N0, float N1) {
    if (N1 == N0) {
        curT = WalkerInfinity;
        deltaT = WalkerInfinity;
    } else {
        deltaT = 1.0f / (N1 - N0);
        if (deltaT < 0) {
            curT = (floor(N0) - N0) * deltaT;
            deltaT = -deltaT;
        } else {
            curT = (ceil(N0) - N0) * deltaT;
        }
    }
}

bool GridCollision::collide(const Point3F& start,
                            const Point3F& end,
                            bool LOS,
                            bool collideWithHoles) {
    tube = false;
    justLOS = LOS;
    hitHoles = collideWithHoles;
    if (transform) {
        Point3F sp, ep;
        m_mul(start, *transform, sp);
        m_mul(end, *transform, ep);
        return collideFile(sp, ep);
    }
    return collideFile(start, end);
}

bool GridCollision::collide(const Point3F& start, const Point3F& end, float in_radius) {
    Point3F sp, ep;
    if (transform) {
        m_mul(start, *transform, sp);
        m_mul(end, *transform, ep);
    } else
        sp = start, ep = end;

    radius = in_radius;
    tube = true;
    justLOS = false;
    hitHoles = false;
    if (collideFile(sp, ep)) {
        return true;
    }
    return false;
}

bool GridCollision::collideFile(const Point3F& start, const Point3F& end) {

    grdSize = terrainFile->getScale();
    blockShift = terrainFile->shift();

    float invBlockSize = 1 / float(1 << (grdSize + blockShift));
    Point3F delta;

    delta = end;
    delta -= start;

    Point3F curStart = start;
    Point2I extent = terrainFile->getSize();

    float curT[3], deltaT[3];

    CalcTs(curT[0], deltaT[0], start.y * invBlockSize, end.y * invBlockSize);
    CalcTs(curT[1], deltaT[1], start.x * invBlockSize, end.x * invBlockSize);
    curT[2] = deltaT[2] = 0;

    float currentT;
    int currentI;
    bool crossed_terrain = false;

    for (;;) {
        collision_detail::spend(work);
        if (curT[0] < curT[1]) {
            currentT = curT[0];
            currentI = 0;
        } else {
            currentT = curT[1];
            currentI = 1;
        }

        if (currentT >= 1.0f) {
            currentT = 1.0f;
            currentI = 2;
        }
        curT[currentI] += deltaT[currentI];

        Point3F cp = delta;
        cp *= currentT;
        cp += start;

        // Starting on an edge, or crossing a corner, can produce tied streams
        // with no interval to test. They do not enter or leave a terrain block.
        if (currentI != 2 && cp == curStart)
            continue;

        float xPos = cp.x * invBlockSize;
        float yPos = cp.y * invBlockSize;
        int ix, iy;

        if (currentI == 0) {

            ix = int(floor(xPos));
            iy = (int)floor(yPos + .5);
            if (delta.y > 0)
                iy--;
        } else if (currentI == 1) {
            ix = (int)floor(xPos + .5);
            iy = int(floor(yPos));
            if (delta.x > 0)
                ix--;
        } else {
            // The endpoint can lie on the next block's boundary. Query the
            // block containing this final interval, including its last edge.
            ix = int(floor((curStart.x + cp.x) * .5f * invBlockSize));
            iy = int(floor((curStart.y + cp.y) * .5f * invBlockSize));
        }
        Point3F blockStart = curStart;
        // Advance through absent and out-of-map blocks too, so an entering
        // segment starts at the block edge rather than outside its height map.
        curStart = cp;
        if (ix >= 0 && ix < extent.x && iy >= 0 && iy < extent.y) {
            const NativeBlock* blk = terrainFile->getBlock(Point2I(ix, iy));
            if (!blk) {
                if (currentI == 2) {
                    return false;
                }
                continue;
            }
            Point3F blockEnd = cp;
            Point3F offset(float(ix * (1 << (grdSize + blockShift))),
                           float(iy * (1 << (grdSize + blockShift))),
                           0);
            blockStart -= offset;
            blockEnd -= offset;
            crossed_terrain = true;

            if (collideBlock(blk, blockStart, blockEnd)) {
                surface.part = blk->getId();
                surface.position += offset;
                return true;
            }
        } else if (crossed_terrain)
            return false;

        if (currentI == 2)
            return false;
    }
}

bool GridCollision::collideBlock(const NativeBlock* block,
                                 const Point3F& start,
                                 const Point3F& end) {

    float curT[4], deltaT[4];

    heightField = block->getHeightMap();
    materialMap = block->getMaterialMap();
    float gridSize;

    gridSize = float(1 << grdSize);

    float invGrdSize = 1 / gridSize;
    float halfInvGrdSize = .5f * invGrdSize;

    int hmwid = block->getHeightMapWidth() - 1;

    Point2I iStart(int(start.x) >> grdSize, int(start.y) >> grdSize);
    Point2I iEnd(int(end.x) >> grdSize, int(end.y) >> grdSize);

    if (iStart.x >= hmwid)
        iStart.x = hmwid - 1;
    else if (iStart.x < 0)
        iStart.x = 0;
    if (iStart.y >= hmwid)
        iStart.y = hmwid - 1;
    else if (iStart.y < 0)
        iStart.y = 0;

    if (iEnd.x >= hmwid)
        iEnd.x = hmwid - 1;
    else if (iEnd.x < 0)
        iEnd.x = 0;
    if (iEnd.y >= hmwid)
        iEnd.y = hmwid - 1;
    else if (iEnd.y < 0)
        iEnd.y = 0;

    GridHeight ht;

    ht.width = float(1 << grdSize);
    ht.dx = 1;
    ht.dy = block->getHeightMapWidth();

    ht.split = ((iStart.x ^ iStart.y) & 1) ? GridHeight::Split135 : GridHeight::Split45;
    ht.heights = block->getHeight(0, iStart);
    Point2F inspos(start.x - (iStart.x * (1 << grdSize)), start.y - (iStart.y * (1 << grdSize)));

    float startHeight = ht.getHeight(inspos);

    ht.split = ((iEnd.x ^ iEnd.y) & 1) ? GridHeight::Split135 : GridHeight::Split45;
    ht.heights = block->getHeight(0, iEnd);
    inspos.set(end.x - (iEnd.x * (1 << grdSize)), end.y - (iEnd.y * (1 << grdSize)));

    float endHeight = ht.getHeight(inspos);

    bool above = start.z > startHeight;

    float deltaX = end.x - start.x;
    float deltaY = end.y - start.y;
    float deltaZ = end.z - start.z;

    CalcTs(curT[0], deltaT[0], start.y * invGrdSize, end.y * invGrdSize);
    CalcTs(curT[1], deltaT[1], start.x * invGrdSize, end.x * invGrdSize);
    CalcTs(
        curT[2], deltaT[2], (start.x - start.y) * halfInvGrdSize, (end.x - end.y) * halfInvGrdSize);
    CalcTs(
        curT[3], deltaT[3], (start.x + start.y) * halfInvGrdSize, (end.x + end.y) * halfInvGrdSize);

    float currentT;
    int currentI;

    for (;;) {
        collision_detail::spend(work);
        currentT = 1.0f;
        currentI = -1;
        for (const auto i : std::views::iota(0, 4)) {
            if (curT[i] < currentT) {
                currentT = curT[i];
                currentI = i;
            }
        }
        if (currentI == -1)
            break;

        curT[currentI] += deltaT[currentI];

        surface.position.x = start.x + deltaX * currentT;
        surface.position.y = start.y + deltaY * currentT;
        surface.position.z = start.z + deltaZ * currentT;

        float xPos = surface.position.x * invGrdSize;
        float yPos = surface.position.y * invGrdSize;
        int iy{}, ix{};
        float height{};

        if (tube) {
            float dh;

            switch (currentI) {
                case 0:

                    ix = int(xPos);
                    iy = (int)floor(yPos + .5);
                    dh = GetHeight(ix + 1, iy) - GetHeight(ix, iy);

                    height = GetHeight(ix, iy) + dh * (xPos - ix);
                    break;
                case 1:

                    iy = int(yPos);
                    ix = (int)floor(xPos + .5);
                    dh = GetHeight(ix, iy + 1) - GetHeight(ix, iy);

                    height = GetHeight(ix, iy) + dh * (yPos - iy);
                    break;
                case 2:

                    ix = int(xPos);
                    iy = int(yPos);
                    dh = GetHeight(ix + 1, iy + 1) - GetHeight(ix, iy);

                    height = GetHeight(ix, iy) + dh * (xPos - ix);
                    break;
                case 3:

                    ix = int(xPos);
                    iy = int(yPos);
                    dh = GetHeight(ix + 1, iy) - GetHeight(ix, iy + 1);

                    height = GetHeight(ix, iy + 1) + dh * (xPos - ix);
                    break;
            }

        } else {
            float h1{}, h2{}, pct{};
            switch (currentI) {
                case 0: {

                    ix = int(xPos);
                    iy = (int)floor(yPos + .5);
                    h1 = GetHeight(ix, iy);
                    h2 = GetHeight(ix + 1, iy);
                    pct = (xPos - ix);
                    break;
                }
                case 1: {

                    iy = int(yPos);
                    ix = (int)floor(xPos + .5);
                    h1 = GetHeight(ix, iy);
                    h2 = GetHeight(ix, iy + 1);
                    pct = (yPos - iy);
                    break;
                }
                case 2: {

                    ix = int(xPos);
                    iy = int(yPos);
                    h1 = GetHeight(ix, iy);
                    h2 = GetHeight(ix + 1, iy + 1);
                    pct = (xPos - ix);
                    break;
                }
                case 3: {

                    ix = int(xPos);
                    iy = int(yPos);
                    h1 = GetHeight(ix, iy + 1);
                    h2 = GetHeight(ix + 1, iy);
                    pct = (xPos - ix);
                    break;
                }
            }
            height = h1 + (h2 - h1) * pct;
        }

        if (xPos <= 0 || yPos <= 0 || xPos >= hmwid || yPos >= hmwid) {
            // At a block edge, diagonal streams can select an exterior cell.
            // Sample the actual boundary triangle rather than extending that
            // cell's clamped vertices and manufacturing a crossing at t=0.
            const Point2I cell{std::clamp(int(xPos), 0, hmwid - 1),
                               std::clamp(int(yPos), 0, hmwid - 1)};
            GridHeight boundary{gridSize,
                                ((cell.x ^ cell.y) & 1) ? GridHeight::Split135
                                                       : GridHeight::Split45,
                                block->getHeight(0, cell),
                                1,
                                block->getHeightMapWidth()};
            height = boundary.getHeight({surface.position.x - cell.x * gridSize,
                                         surface.position.y - cell.y * gridSize});
        }

        if (int(surface.position.z > height) ^ above) {

            if (currentI == 0 && deltaY > 0)
                iy--;
            else if (currentI == 1 && deltaX > 0)
                ix--;

            // Crossing streams can meet on the outer row/column (including
            // t=0). The boundary vertices belong to the last interior cell.
            ix = std::clamp(ix, 0, hmwid - 1);
            iy = std::clamp(iy, 0, hmwid - 1);

            if (!hitHoles && GetMaterial(ix, iy)->emptyLevel()) {
                above = !above;
                continue;
            }

            surface.position.z = height;

            if (justLOS)
                return (true);

            enum splittypes { lower45, upper45, lower135, upper135 };

            int spt{};
            switch (currentI) {
                case 0:

                    if (deltaY < 0) {
                        if ((ix ^ iy) & 1)
                            spt = lower135;
                        else
                            spt = lower45;
                    } else {
                        if ((ix ^ iy) & 1)
                            spt = upper135;
                        else
                            spt = upper45;
                    }
                    break;
                case 1:

                    if (deltaX < 0) {
                        if ((ix ^ iy) & 1)
                            spt = lower135;
                        else
                            spt = upper45;
                    } else {
                        if ((ix ^ iy) & 1)
                            spt = upper135;
                        else
                            spt = lower45;
                    }
                    break;
                case 2:

                    if (deltaX > deltaY)
                        spt = upper45;
                    else
                        spt = lower45;
                    break;
                case 3:

                    if (deltaX > -deltaY)
                        spt = lower135;
                    else
                        spt = upper135;
                    break;
            }

            PlaneF plane;
            Point3F normal;
            ht.heights = block->getHeight(0, Point2I(ix, iy));
            switch (spt) {
                case upper45:
                    ht.getNormal45a(&normal);
                    plane.set(GetPoint(ix, iy), normal);
                    break;
                case lower45:
                    ht.getNormal45b(&normal);
                    plane.set(GetPoint(ix, iy), normal);
                    break;
                case lower135:
                    ht.getNormal135a(&normal);
                    plane.set(GetPoint(ix, iy + 1), normal);
                    break;
                case upper135:
                    ht.getNormal135b(&normal);
                    plane.set(GetPoint(ix, iy + 1), normal);
                    break;
            }
            LineSeg3F seg(start, end);
            if (tube)
                plane.d += above ? radius : -radius;

            (void)plane.intersect(seg, surface.position);

            surface.normal = plane;
            surface.distance = 0.0f;
            surface.surface = 0;
            surface.material = GetMaterial(ix, iy)->index;
            return true;
        }
    }

    if (tube)
        endHeight += above ? radius : -radius;

    if (above != (end.z > endHeight)) {
        const TerrainGridMaterial* mat = GetMaterial(iEnd.x, iEnd.y);
        if (mat->emptyLevel())
            return false;

        Point3F soffset(float(iEnd.x * (1 << grdSize)), float(iEnd.y * (1 << grdSize)), 0);
        Point3F oStart = start;
        oStart -= soffset;
        Point3F oEnd = end;
        oEnd -= soffset;

        bool ret;
        LineSeg3F seg(oStart, oEnd);

        ret = ht.intersect(seg, &surface.position);
        surface.distance = 0.0f;
        surface.surface = 0;
        surface.material = mat->index;

        if (ret) {
            ht.getNormal({surface.position.x, surface.position.y}, &surface.normal);
            surface.normal.normalize();
        }
        surface.position += soffset;
        return ret;
    }

    return (false);
}

void checkNative(Point3F p) {
    collision_detail::check(p, {});
    if (std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 100'000'000.f) {
        throw std::invalid_argument("native terrain coordinate limit");
    }
}

bool segment(const TerrainGridResources& r,
             Point3F a,
             Point3F b,
             float radius,
             bool tube,
             bool editor,
             CollisionSurfaceList& out) {
    checkNative(a);
    checkNative(b);
    collision_detail::check(a, b, radius);
    NativeFile file(r);
    GridCollision walker;
    walker.terrainFile = &file;
    const auto hit = tube ? walker.collide(a, b, radius) : walker.collide(a, b, false, editor);
    if (!hit) {
        return false;
    }
    // Native line/tube code leaves time unwritten. Publish a deterministic time
    // for modern sweep consumers without changing the native contact position.
    const auto delta = b - a;
    const auto length = m_dot(delta, delta);
    walker.surface.time = length ? m_dot(walker.surface.position - a, delta) / length : 0.f;
    CollisionSurfaceList result;
    result.surfaces.push_back(walker.surface);
    return collision_detail::publish(out, std::move(result));
}
} // namespace

std::optional<TerrainCollisionSnapshot> TerrainCollisionSnapshot::prepare(TerrainGridResources r) {
    if (!r.grid || r.grid->detailCount < 1 || r.grid->detailCount > 11 || r.grid->scale < 0 ||
        r.grid->scale > 16 || r.grid->size.x < 1 || r.grid->size.y < 1 || r.grid->size.x > 256 ||
        r.grid->size.y > 256 ||
        r.grid->blockMap.size() != UInt64(r.grid->size.x) * r.grid->size.y ||
        r.grid->blockMap.size() > 4096 || r.blocks.size() != r.grid->blocks.size() ||
        r.blocks.size() > 4096) {
        return {};
    }
    const auto n = 1U << (r.grid->detailCount - 1);
    std::map<Int32, bool> ids;
    std::size_t heights{};
    for (auto i : std::views::iota(std::size_t{0}, r.blocks.size())) {
        if (!ids.emplace(r.grid->blocks[i].id, true).second || r.grid->blocks[i].id < 0) {
            return {};
        }
        if (const auto& b = r.blocks[i]) {
            if (b->width != n || b->detailCount != r.grid->detailCount ||
                b->heights.size() != UInt64(n + 1) * (n + 1) ||
                b->materials.size() != UInt64(n) * n || b->heights.size() > 4'194'304 - heights ||
                std::ranges::any_of(b->heights, [](float v) {
                    return !std::isfinite(v) || std::abs(v) > 1e8f;
                })) {
                return {};
            }
            heights += b->heights.size();
        }
    }
    if (std::ranges::any_of(r.grid->blockMap, [&](auto id) {
            return id != -1 && !ids.contains(id);
        })) {
        return {};
    }
    TerrainCollisionSnapshot result;
    const auto blockSize = Flt32(1U << (r.grid->scale + r.grid->detailCount - 1));
    result.bounds_ = {{0, 0, -1e8f},
                      {blockSize * r.grid->size.x, blockSize * r.grid->size.y, 1e8f}};
    result.resources_ = std::move(r);
    return result;
}

std::optional<CollisionSurface> TerrainCollisionSnapshot::surface(Point2F p) const {
    checkNative({p.x, p.y, 0});
    const auto& grid = *resources_.grid;
    const int shift = grid.detailCount - 1 + grid.scale;
    // Native truncates before arithmetic shifting, including coordinates in (-1,0).
    const Point2I bp{Int32(p.x) >> shift, Int32(p.y) >> shift};
    if (bp.x < 0 || bp.y < 0 || bp.x >= grid.size.x || bp.y >= grid.size.y) {
        return {};
    }
    NativeFile file(resources_);
    const auto* b = file.getBlock(bp);
    if (!b) {
        return {};
    }
    const Point2F local{p.x - float(bp.x * (1 << shift)), p.y - float(bp.y * (1 << shift))};
    const Point2I cell{Int32(local.x) >> grid.scale, Int32(local.y) >> grid.scale};
    if (cell.x < 0 || cell.y < 0 || cell.x >= int(b->block->width) ||
        cell.y >= int(b->block->width)) {
        return {};
    }
    const auto material = b->block->materials[std::size_t(cell.y) * b->block->width + cell.x];
    if (material.hole()) {
        return {};
    }
    GridHeight h{float(1 << grid.scale),
                 ((cell.x ^ cell.y) & 1) ? GridHeight::Split135 : GridHeight::Split45,
                 b->getHeight(0, cell),
                 1,
                 b->getHeightMapWidth()};
    Point2F off{local.x - float(cell.x * (1 << grid.scale)),
                local.y - float(cell.y * (1 << grid.scale))};
    CollisionSurface result;
    result.position = {p.x, p.y, h.getHeight(off)};
    h.getNormal(off, &result.normal);
    result.normal.normalize();
    result.material = material.index;
    result.surface = 0;
    result.part = b->id;
    result.time = 1;
    return result;
}

bool TerrainCollisionSnapshot::line(Point3F a,
                                    Point3F b,
                                    CollisionSurfaceList& out,
                                    bool editor) const {
    return segment(resources_, a, b, 0, false, editor, out);
}

bool TerrainCollisionSnapshot::tube(Point3F a,
                                    Point3F b,
                                    float radius,
                                    CollisionSurfaceList& out) const {
    return segment(resources_, a, b, radius, true, false, out);
}

bool TerrainCollisionSnapshot::sphere(Point3F center,
                                      float radius,
                                      CollisionSurfaceList& out) const {
    checkNative(center);
    collision_detail::check(center, {}, radius);
    auto hit = surface({center.x, center.y});
    if (!hit) {
        return false;
    }
    const auto distance = std::abs(m_dot(center, hit->normal) - m_dot(hit->position, hit->normal));
    if (distance >= radius) {
        return false;
    }
    hit->distance = distance;
    CollisionSurfaceList result;
    result.surfaces.push_back(*hit);
    return collision_detail::publish(out, std::move(result));
}

bool TerrainCollisionSnapshot::box(const Box3F& b, const TMat3F& t, CollisionSurfaceList&) const {
    if (!collision_detail::valid(b) || !collision_detail::rigid(t)) {
        throw std::invalid_argument("invalid native terrain box");
    }
    return false;
}

bool TerrainCollisionSnapshot::shape(Point3F center,
                                     float bottom,
                                     const TMat3F& toShape,
                                     CollisionSurfaceList& out) const {
    if (!collision_detail::rigid(toShape) || !std::isfinite(bottom)) {
        throw std::invalid_argument("invalid native TS support");
    }
    auto inverse = toShape;
    inverse.inverse();
    const auto pos = collision_detail::point(center, inverse);
    auto hit = surface({pos.x, pos.y});
    if (!hit) {
        return false;
    }
    hit->position = collision_detail::point(hit->position, toShape);
    const auto height = bottom - hit->position.z;
    if (height >= 0) {
        return false;
    }
    hit->distance = -height;
    CollisionSurfaceList result;
    result.surfaces.push_back(*hit);
    return collision_detail::publish(out, std::move(result));
}
} // namespace opent1::world
