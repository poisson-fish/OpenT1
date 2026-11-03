// Port of native terrData/terrRender numerical routines. Dynamix, 1997.
// Checked storage and documented boundary repairs in terrain-authoring.md.
static UInt16 calcDev(PlaneF& pl, Point3F& pt) {
    float z = (pl.d + pl.x * pt.x + pl.y * pt.y) / -pl.z;
    float diff = z - pt.z;
    if (diff < 0)
        diff = -diff;

    if (diff > 0xFFFF)
        return 0xFFFF;
    else
        return UInt16(diff);
}

UInt16 Umax(UInt16 u1, UInt16 u2) {
    return u1 > u2 ? u1 : u2;
}

void NativeTerrainBlock::buildGridMap() {
    for (int i = BlockShift; i >= 0; i--) {
        int squareCount = 1 << (BlockShift - i);
        int squareSize = (NativeTerrainBlock::BlockSize - 1) / squareCount;

        for (auto squareX : std::views::iota(0) | std::views::take_while([&](int squareX) {
                                return squareX < squareCount;
                            })) {
            for (auto squareY : std::views::iota(0) | std::views::take_while([&](int squareY) {
                                    return squareY < squareCount;
                                })) {
                UInt16 min = 0xFFFF;
                UInt16 max = 0;
                UInt16 mindev45 = 0;
                UInt16 mindev135 = 0;

                Point3F p1, p2, p3, p4;

                PlaneF pl1, pl2, pl3, pl4;

                p1.set(0, 0, getHeight(squareX * squareSize, squareY * squareSize));
                p2.set(0,
                       squareSize,
                       getHeight(squareX * squareSize, squareY * squareSize + squareSize));
                p3.set(squareSize,
                       squareSize,
                       getHeight(squareX * squareSize + squareSize,
                                 squareY * squareSize + squareSize));
                p4.set(squareSize,
                       0,
                       getHeight(squareX * squareSize + squareSize, squareY * squareSize));

                pl1.set(p1, p2, p3);
                pl2.set(p1, p3, p4);
                pl3.set(p1, p2, p4);
                pl4.set(p2, p3, p4);
                bool parentSplit45 = false;
                GridSquare* parent = nullptr;
                if (i < BlockShift) {
                    parent = findSquare(i + 1, Point2I(squareX * squareSize, squareY * squareSize));
                    parentSplit45 = parent->flags & GridSquare::Split45;
                }
                bool empty = true;

                for (auto sizeX : std::views::iota(0) | std::views::take_while([&](int sizeX) {
                                      return sizeX <= squareSize;
                                  })) {
                    for (auto sizeY : std::views::iota(0) | std::views::take_while([&](int sizeY) {
                                          return sizeY <= squareSize;
                                      })) {
                        int x = squareX * squareSize + sizeX;
                        int y = squareY * squareSize + sizeY;

                        if (sizeX != squareSize && sizeY != squareSize) {
                            GridBlock::Material* mat = getMaterial(x, y);
                            if (mat->emptyLevel() == 0)
                                empty = false;
                        }
                        UInt16 ht = heightMap[x + y + (y << BlockShift)];

                        if (ht < min)
                            min = ht;
                        if (ht > max)
                            max = ht;
                        Point3F pt(sizeX, sizeY, fixedToFloat(ht));
                        UInt16 dev;

                        if (sizeX < sizeY)
                            dev = calcDev(pl1, pt);
                        else if (sizeX > sizeY)
                            dev = calcDev(pl2, pt);
                        else
                            dev = Umax(calcDev(pl1, pt), calcDev(pl2, pt));

                        if (dev > mindev45)
                            mindev45 = dev;

                        if (sizeX + sizeY < squareSize)
                            dev = calcDev(pl3, pt);
                        else if (sizeX + sizeY > squareSize)
                            dev = calcDev(pl4, pt);
                        else
                            dev = Umax(calcDev(pl3, pt), calcDev(pl4, pt));

                        if (dev > mindev135)
                            mindev135 = dev;
                    }
                }
                GridSquare* sq = findSquare(i, Point2I(squareX * squareSize, squareY * squareSize));
                sq->minimum = min;
                sq->maximum = max;

                sq->flags = empty ? GridSquare::Empty : 0;

                bool shouldSplit45 = ((squareX ^ squareY) & 1) == 0;
                bool split45;

                if (i == 0)
                    split45 = shouldSplit45;
                else if (i < 4 && shouldSplit45 == parentSplit45)
                    split45 = shouldSplit45;
                else
                    split45 = mindev45 < mindev135;

                if (split45) {
                    sq->flags |= GridSquare::Split45;
                    sq->deviance = mindev45;
                } else
                    sq->deviance = mindev135;
                if (parent)
                    if (parent->deviance < sq->deviance)
                        parent->deviance = sq->deviance;
            }
        }
    }
}

void NativeTerrainRender::setupColorAdder(float rSquared, float r, float g, float b) {
    currentR = r;
    currentG = g;
    currentB = b;
    currentRSq = rSquared;
}

void NativeTerrainRender::addColor(Color& c, float dSquared) {
    if (dSquared > currentRSq)
        return;
    float scale = 255.0f * (1 - (dSquared / currentRSq));
    c.r += currentR * scale;
    c.g += currentG * scale;
    c.b += currentB * scale;
}

inline UInt16 NativeTerrainRender::convertColorBack(Color& c) {
    UInt16 ret;
    if (c.r > 255)
        ret = 0xF800;
    else
        ret = (c.r >> 3) << 11;
    if (c.g > 255)
        ret |= 0x07E0;
    else
        ret |= (c.g >> 2) << 5;

    if (c.b > 255)
        ret |= 0x001F;
    else
        ret |= c.b >> 3;

    return ret;
}

inline void NativeTerrainRender::convertColor(Color& c, UInt16 lix) {
    int r = lix >> 11;
    int g = (lix >> 5) & 0x3F;
    int b = lix & 0x1F;

    c.r = (r << 3) | (r >> 2);
    c.g = (g << 2) | (g >> 4);
    c.b = (b << 3) | (b >> 2);
}

void NativeTerrainRender::lightExpandHorz(
    float x, float ySquared, float dx, int j, int i, int shift) {
    int size = 1 << shift;
    float scale = 1 / float(size);

    float z = trs.lmaBuffer[j][i].z;
    float dz = (trs.lmaBuffer[j][i + size].z - z) * scale;

    for (auto count : std::views::iota(1) | std::views::take_while([&](int count) {
                          return count < size;
                      })) {
        x += dx;
        z += dz;

        addColor(trs.lmaBuffer[j][i + count], x * x + z * z + ySquared);
    }
}

void NativeTerrainRender::lightExpandVert(
    float y, float xSquared, float dy, int j, int i, int shift) {
    int size = 1 << shift;
    float scale = 1 / float(size);

    float z = trs.lmaBuffer[j][i].z;
    float dz = (trs.lmaBuffer[j + size][i].z - z) * scale;

    for (auto count : std::views::iota(1) | std::views::take_while([&](int count) {
                          return count < size;
                      })) {
        y += dy;
        z += dz;

        trs.lmaBuffer[j + count][i].z = z;
        addColor(trs.lmaBuffer[j + count][i], y * y + z * z + xSquared);
    }
}

void NativeTerrainRender::lmExpandHorz(int y, int x, int shift) {
    int size = 1 << shift;
    Color d;
    Color c = trs.lmaBuffer[y][x];
    d.r = (trs.lmaBuffer[y][x + size].r - c.r) >> shift;
    d.g = (trs.lmaBuffer[y][x + size].g - c.g) >> shift;
    d.b = (trs.lmaBuffer[y][x + size].b - c.b) >> shift;

    for (auto i : std::views::iota(1) | std::views::take_while([&](int i) {
                      return i < size;
                  })) {
        c.r += d.r;
        c.g += d.g;
        c.b += d.b;

        trs.lmaBuffer[y][x + i] = c;
    }
}

void NativeTerrainRender::lmExpandVert(int y, int x, int shift) {
    int size = 1 << shift;
    Color d;
    Color c = trs.lmaBuffer[y][x];
    d.r = (trs.lmaBuffer[y + size][x].r - c.r) >> shift;
    d.g = (trs.lmaBuffer[y + size][x].g - c.g) >> shift;
    d.b = (trs.lmaBuffer[y + size][x].b - c.b) >> shift;

    for (auto i : std::views::iota(1) | std::views::take_while([&](int i) {
                      return i < size;
                  })) {
        c.r += d.r;
        c.g += d.g;
        c.b += d.b;

        trs.lmaBuffer[y + i][x] = c;
    }
}

void NativeTerrainRender::expandDynamicLight(TerrLightInfo& tl, int x, int y, int level) {
    setupColorAdder(tl.radiusSquared, tl.r, tl.g, tl.b);

    if (DynamicLightMapShift - level <= 0) {
        int step = 1 << (level - DynamicLightMapShift);
        int count = DynamicLightMapSize;
        for (const auto j : std::views::iota(0, count + 1)) {
            for (const auto i : std::views::iota(0, count + 1)) {
                float xp = (x + i * step) * trs.squareSize - tl.pos.x;
                float yp = (y + j * step) * trs.squareSize - tl.pos.y;
                float zp = fixedToFloat(trs.currentBlock->getHeight(
                               (x + i) & NativeTerrainBlock::BlockMask,
                               (y + j) & NativeTerrainBlock::BlockMask)) -
                           tl.pos.z;

                addColor(trs.lmaBuffer[j][i], xp * xp + yp * yp + zp * zp);
            }
        }
    } else {
        int delt = DynamicLightMapShift - level;
        int size = 1 << delt;
        int count = 1 << level;

        float delta = trs.squareSize / float(size);

        for (const auto j : std::views::iota(0, count + 1)) {
            for (const auto i : std::views::iota(0, count + 1)) {
                float xp = (x + i) * trs.squareSize - tl.pos.x;
                float yp = (y + j) * trs.squareSize - tl.pos.y;
                float zp = fixedToFloat(trs.currentBlock->getHeight(
                               (x + i) & NativeTerrainBlock::BlockMask,
                               (y + j) & NativeTerrainBlock::BlockMask)) -
                           tl.pos.z;
                trs.lmaBuffer[j << delt][i << delt].z = zp;

                addColor(trs.lmaBuffer[j << delt][i << delt], xp * xp + yp * yp + zp * zp);
                if (i != 0)
                    lightExpandHorz(
                        xp - trs.squareSize, yp * yp, delta, j << delt, (i - 1) << delt, delt);
                if (j != 0)
                    lightExpandVert(
                        yp - trs.squareSize, xp * xp, delta, (j - 1) << delt, i << delt, delt);
                if (i != 0 && j != 0) {
                    float previousX = (x + i - 1) * trs.squareSize - tl.pos.x;
                    for (auto k : std::views::iota(1) | std::views::take_while([&](int k) {
                                      return k < size;
                                  })) {
                        float rowY = (y + j - 1) * trs.squareSize + k * delta - tl.pos.y;
                        lightExpandHorz(previousX,
                                        rowY * rowY,
                                        delta,
                                        ((j - 1) << delt) + k,
                                        (i - 1) << delt,
                                        delt);
                    }
                }
            }
        }
    }
}

void NativeTerrainRender::expandLightMap(int x, int y, int level) {
    x &= NativeTerrainBlock::BlockMask;
    y &= NativeTerrainBlock::BlockMask;
    if (DynamicLightMapShift - level <= 0) {
        int step = (level - DynamicLightMapShift);
        int count = DynamicLightMapSize;
        int lightMask = trs.currentBlock->lightMap->getWidth() - 1;

        for (const auto j : std::views::iota(0, count + 1)) {
            for (const auto i : std::views::iota(0, count + 1)) {
                UInt16 lix = *((UInt16*)trs.currentBlock->lightMap->getAddress(
                    (x + (i << step)) & lightMask, (y + (j << step)) & lightMask));
                convertColor(trs.lmaBuffer[j][i], lix);
            }
        }
    } else {
        int delt = DynamicLightMapShift - level;
        int size = 1 << delt;
        int count = 1 << level;

        for (const auto j : std::views::iota(0, count + 1)) {
            UInt16* pixPtr =
                (UInt16*)trs.currentBlock->lightMap->getAddress(x & 255, (y + j) & 255);
            for (const auto i : std::views::iota(0, count + 1)) {
                convertColor(trs.lmaBuffer[j << delt][i << delt], *pixPtr++);
                if (i != 0)
                    lmExpandHorz(j << delt, (i - 1) << delt, delt);
                if (j != 0)
                    lmExpandVert((j - 1) << delt, i << delt, delt);
                if (i != 0 && j != 0) {
                    for (auto k : std::views::iota(1) | std::views::take_while([&](int k) {
                                      return k < size;
                                  }))
                        lmExpandHorz(((j - 1) << delt) + k, (i - 1) << delt, delt);
                }
            }
        }
    }
}

void NativeTerrainRender::collapseLightMap() {
    for (const auto j : std::views::iota(0, 9)) {
        UInt16* row = (UInt16*)trs.currentBlock->dynamicLightMap->getAddress(0, j);
        Color* cwalk = trs.lmaBuffer[j];
        for ([[maybe_unused]] const auto i : std::views::iota(0, 9))
            *row++ = convertColorBack(*cwalk++);
    }
}

UInt32
NativeTerrainRender::TestSquareLights(GridSquare* sq, int level, Point2I pos, UInt32 lightMask) {
    UInt32 retMask = 0;
    float blockX = pos.x * trs.squareSize + trs.blockPos.x;
    float blockY = pos.y * trs.squareSize + trs.blockPos.y;
    float blockZ = fixedToFloat(sq->minimum);

    float blockSize = trs.squareSize * (1 << level);
    float blockHeight = fixedToFloat(sq->maximum - sq->minimum);

    Point3F vec;

    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return (lightMask >> i) != 0;
                  })) {

        if (lightMask & (1 << i)) {
            const Point3F& lightPos = terrainLights[i].pos;

            if (lightPos.z < blockZ)
                vec.z = blockZ - lightPos.z;
            else if (lightPos.z > blockZ + blockHeight)
                vec.z = lightPos.z - (blockZ + blockHeight);
            else
                vec.z = 0;

            if (lightPos.x < blockX)
                vec.x = blockX - lightPos.x;
            else if (lightPos.x > blockX + blockSize)
                vec.x = lightPos.x - (blockX + blockSize);
            else
                vec.x = 0;

            if (lightPos.y < blockY)
                vec.y = blockY - lightPos.y;
            else if (lightPos.y > blockY + blockSize)
                vec.y = lightPos.y - (blockY + blockSize);
            else
                vec.y = 0;
            float dist = vec.len();
            if (dist < terrainLights[i].radius)
                retMask |= (1 << i);
        }
    }
    return retMask;
}

int NativeTerrainRender::TestSquareVisibility(Point3F& min, Point3F& max, int mask, float expand) {
    int retMask = 0;
    Point3F minPoint, maxPoint;
    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return i < NumClipPlanes;
                  })) {
        if (mask & (1 << i)) {
            if (trs.clipPlane[i].x > 0) {
                maxPoint.x = max.x;
                minPoint.x = min.x;
            } else {
                maxPoint.x = min.x;
                minPoint.x = max.x;
            }
            if (trs.clipPlane[i].y > 0) {
                maxPoint.y = max.y;
                minPoint.y = min.y;
            } else {
                maxPoint.y = min.y;
                minPoint.y = max.y;
            }
            if (trs.clipPlane[i].z > 0) {
                maxPoint.z = max.z;
                minPoint.z = min.z;
            } else {
                maxPoint.z = min.z;
                minPoint.z = max.z;
            }
            float maxDot = m_dot(maxPoint, trs.clipPlane[i]);
            float minDot = m_dot(minPoint, trs.clipPlane[i]);
            float planeD = trs.clipPlane[i].d;
            if (maxDot <= -(planeD + expand))
                return -1;
            if (minDot <= -planeD)
                retMask |= (1 << i);
        }
    }
    return retMask;
}

int NativeTerrainRender::allocInitialPoint(Point3F pos) {
    if (trs.pointCount >= MaxScanPoints) {
        throw std::runtime_error("Terrain2 point limit");
    }
    int ret = trs.pointCount++;
    trs.points[ret].minGrowFactor = 0;
    trs.points[ret].pointHeight = pos.z;
    trs.points[ret].avgHeight = pos.z;
    trs.emitPoints[ret].x = pos.x;
    trs.emitPoints[ret].y = pos.y;
    trs.points[ret].p1 = 0;
    trs.points[ret].p2 = 0;
    trs.waterTexCoords[ret].set(pos.x * trs.texScale.x, pos.y * trs.texScale.y);
    return ret;
}

int NativeTerrainRender::allocWaterPoint(Point2F pos, int p1, int p2) {
    if (trs.pointCount >= MaxScanPoints) {
        throw std::runtime_error("Terrain2 point limit");
    }
    int ret = trs.pointCount++;
    trs.points[ret].minGrowFactor = trs.currentGrowFactor;
    trs.points[ret].pointHeight = getWaterHeight(pos, ret);
    trs.points[ret].avgHeight = 0;
    trs.emitPoints[ret].x = pos.x;
    trs.emitPoints[ret].y = pos.y;
    trs.points[ret].p1 = p1;
    trs.points[ret].p2 = p2;
    trs.waterTexCoords[ret].set(pos.x * trs.texScale.x, pos.y * trs.texScale.y);
    return ret;
}

int NativeTerrainRender::allocPoint(Point2I pos, int p1, int p2) {
    if (trs.pointCount >= MaxScanPoints) {
        throw std::runtime_error("Terrain2 point limit");
    }
    int ret = trs.pointCount++;
    trs.points[ret].minGrowFactor = trs.currentGrowFactor;
    trs.points[ret].pointHeight = fixedToFloat(
        *(trs.currentBlock->heightMap + pos.x + pos.y + (pos.y << NativeTerrainBlock::BlockShift)));
    trs.points[ret].avgHeight = 0;
    trs.emitPoints[ret].x = (trs.blockOffset.x + pos.x) * trs.squareScale.x;
    trs.emitPoints[ret].y = (trs.blockOffset.y + pos.y) * trs.squareScale.y;
    trs.points[ret].p1 = p1;
    trs.points[ret].p2 = p2;
    return ret;
}

int NativeTerrainRender::allocEdges(int count) {
    if (count < 0 || count > MaxScanEdges - trs.edgeCount) {
        throw std::runtime_error("Terrain2 edge limit");
    }
    int ret = trs.edgeCount;
    trs.edgeCount += count;

    for (auto i : std::views::iota(static_cast<int>(ret)) | std::views::take_while([&](int i) {
                      return i < ret + count;
                  }))
        trs.edges[i].mp = InvalidPointIndex;
    return ret;
}

void NativeTerrainRender::clampEdge(int edge) {
    std::array<int, EdgeStackSize> edgeStack;
    int stackSize = 0;
    for (;;) {
        int mp = trs.edges[edge].mp;
        if (mp < InvalidPointIndex) {
            trs.points[mp].minGrowFactor = 1;
            edgeStack[stackSize++] = trs.edges[edge].firstSubEdge + 1;
            edge = trs.edges[edge].firstSubEdge;
            continue;
        } else
            trs.edges[edge].mp = ClampEdgeIndex;
        if (!stackSize--)
            break;
        edge = edgeStack[stackSize];
    }
}

void NativeTerrainRender::subdivideEdge(int edge, Point2I pos) {
    int mp = trs.edges[edge].mp;
    if (mp >= InvalidPointIndex) {
        int p1 = trs.edges[edge].p1;
        int p2 = trs.edges[edge].p2;
        int midPoint = allocPoint(pos, p1, p2);
        int fse = allocEdges(2);

        if (mp == ClampEdgeIndex) {
            trs.points[midPoint].minGrowFactor = 1;
            trs.edges[fse].mp = ClampEdgeIndex;
            trs.edges[fse + 1].mp = ClampEdgeIndex;
        }
        trs.edges[edge].mp = midPoint;
        trs.edges[edge].firstSubEdge = fse;
        trs.edges[fse].p1 = trs.edges[edge].p1;
        trs.edges[fse].p2 = midPoint;
        trs.edges[fse + 1].p1 = midPoint;
        trs.edges[fse + 1].p2 = trs.edges[edge].p2;
    } else {
        trs.points[mp].minGrowFactor = max(trs.points[mp].minGrowFactor, trs.currentGrowFactor);
    }
}

void NativeTerrainRender::subdivideWaterEdge(int edge, Point2F pos) {
    int mp = trs.edges[edge].mp;
    if (mp >= InvalidPointIndex) {
        int p1 = trs.edges[edge].p1;
        int p2 = trs.edges[edge].p2;
        int midPoint = allocWaterPoint(pos, p1, p2);
        int fse = allocEdges(2);

        if (mp == ClampEdgeIndex) {
            trs.points[midPoint].minGrowFactor = 1;
            trs.edges[fse].mp = ClampEdgeIndex;
            trs.edges[fse + 1].mp = ClampEdgeIndex;
        }
        trs.edges[edge].mp = midPoint;
        trs.edges[edge].firstSubEdge = fse;
        trs.edges[fse].p1 = trs.edges[edge].p1;
        trs.edges[fse].p2 = midPoint;
        trs.edges[fse + 1].p1 = midPoint;
        trs.edges[fse + 1].p2 = trs.edges[edge].p2;
    } else {
        trs.points[mp].minGrowFactor = max(trs.points[mp].minGrowFactor, trs.currentGrowFactor);
    }
}

float NativeTerrainRender::getSquareDistance(Point3F& minPoint, Point3F& maxPoint) {
    Point3F vec;
    if (trs.camPos.z < minPoint.z)
        vec.z = minPoint.z - trs.camPos.z;
    else if (trs.camPos.z > maxPoint.z)
        vec.z = trs.camPos.z - maxPoint.z;
    else
        vec.z = 0;

    if (trs.camPos.x < minPoint.x)
        vec.x = minPoint.x - trs.camPos.x;
    else if (trs.camPos.x > maxPoint.x)
        vec.x = trs.camPos.x - maxPoint.x;
    else
        vec.x = 0;

    if (trs.camPos.y < minPoint.y)
        vec.y = minPoint.y - trs.camPos.y;
    else if (trs.camPos.y > maxPoint.y)
        vec.y = trs.camPos.y - maxPoint.y;
    else
        vec.y = 0;

    return vec.len();
}

void NativeTerrainRender::emitWaterSquare(WaterStackNode* n) {
    if (trs.squareCount >= MaxEmitSquares) {
        throw std::runtime_error("Terrain2 square limit");
    }
    int square = trs.squareCount++;
    trs.squares[square].edge[0] = n->top;
    trs.squares[square].edge[1] = n->right;
    trs.squares[square].edge[2] = n->bottom;
    trs.squares[square].edge[3] = n->left;
}

void NativeTerrainRender::emitSquare(SquareStackNode* n,
                                     int flags,
                                     int mipLevel,
                                     UInt32 lightMask) {
    if (trs.squareCount >= MaxEmitSquares) {
        throw std::runtime_error("Terrain2 square limit");
    }
    int square = trs.squareCount++;
    if (lightMask)
        flags |= EmitSquare::DynamicallyLit;

    trs.squares[square].edge[0] = n->top;
    trs.squares[square].edge[1] = n->right;
    trs.squares[square].edge[2] = n->bottom;
    trs.squares[square].edge[3] = n->left;
    trs.squares[square].level = n->level;
    trs.squares[square].clipFlags = n->clipFlags;
    trs.squares[square].x = n->pos.x + trs.blockOffset.x;
    trs.squares[square].y = n->pos.y + trs.blockOffset.y;
    trs.squares[square].flags = flags;
    trs.squares[square].mipLevel = mipLevel;
    trs.squares[square].lightMask = lightMask;
}

void NativeTerrainRender::processCurrentBlock(int topEdge,
                                              int rightEdge,
                                              int bottomEdge,
                                              int leftEdge) {
    SquareStackNode stack[NativeTerrainBlock::BlockShift * 4];
    Point3F minPoint, maxPoint;

    stack[0].level = NativeTerrainBlock::BlockShift;
    stack[0].clipFlags = 0xFF;
    stack[0].pos.set(0, 0);
    stack[0].top = topEdge;
    stack[0].right = rightEdge;
    stack[0].bottom = bottomEdge;
    stack[0].left = leftEdge;
    stack[0].lightMask = (1U << trs.dynamicLightCount) - 1;

    int curStackSize = 1;
    float squareDistance;

    float ps = trs.screenSize / trs.camera->getPixelScale();
    float distanceScale = trs.camera->projectionDistance(ps, 1);

    while (curStackSize) {
        SquareStackNode* n = stack + curStackSize - 1;

        int clipFlags = n->clipFlags;
        Point2I pos = n->pos;
        GridSquare* sq = trs.currentBlock->findSquare(n->level, pos);

        minPoint.set(trs.squareScale.x * pos.x + trs.blockPos.x,
                     trs.squareScale.y * pos.y + trs.blockPos.y,
                     fixedToFloat(sq->minimum));
        maxPoint.set(minPoint.x + (trs.squareSize << n->level),
                     minPoint.y + (trs.squareSize << n->level),
                     fixedToFloat(sq->maximum));

        squareDistance = getSquareDistance(minPoint, maxPoint);

        if (squareDistance >= trs.farDistance || (sq->flags & GridSquare::Empty)) {
            curStackSize--;
            continue;
        }
        if (clipFlags) {

            float zDelta = squareDistance * trs.pixelError;
            minPoint.z -= zDelta;
            maxPoint.z += zDelta;

            clipFlags = TestSquareVisibility(minPoint, maxPoint, clipFlags, trs.squareSize);
            if (clipFlags == -1) {

                curStackSize--;
                continue;
            }
        }
        UInt32 lightMask = n->lightMask;
        if (lightMask)
            lightMask = TestSquareLights(sq, n->level, n->pos, lightMask);

        if (n->level == 0) {

            GridBlock::Material* material = trs.currentBlock->getMaterial(n->pos.x, n->pos.y);
            emitSquare(n, sq->flags, material->index, lightMask);
            curStackSize--;
            continue;
        }

        trs.currentGrowFactor = 0;

        bool subdivideSquare = false;

        if (squareDistance < 1)
            subdivideSquare = true;
        else {
            float squareSize = trs.squareSize << n->level;
            float detailDistance = (distanceScale * squareSize) - squareSize;

            if (squareDistance < detailDistance) {
                subdivideSquare = true;
                float clampDistance = 0.75 * detailDistance;

                if (squareDistance > clampDistance)
                    trs.currentGrowFactor =
                        (squareDistance - clampDistance) / (0.25 * detailDistance);
            }
        }
        if (!subdivideSquare || trs.edgeCount > ScanEdgeLimit || trs.pointCount > ScanPointLimit) {
            clampEdge(n->top);
            clampEdge(n->right);
            clampEdge(n->bottom);
            clampEdge(n->left);

            int mipLevel = 0;
            if (squareDistance > 0.001) {
                int squareSz = trs.squareSize << n->level;
                int size =
                    int(trs.camera->projectRadius(squareDistance + (squareSz >> 1), squareSz));
                mipLevel = getPower(size) - 1;
            }
            if (mipLevel > 6)
                mipLevel = 6;
            if (mipLevel <= n->level + 1)
                mipLevel = 0xFF;
            emitSquare(n, sq->flags, mipLevel, lightMask);
            curStackSize--;
            continue;
        }

        int p1, p2;
        int top = n->top;
        int right = n->right;
        int bottom = n->bottom;
        int left = n->left;

        int squareOneSize = 1 << n->level;
        int squareHalfSize = squareOneSize >> 1;

        if (sq->flags & GridSquare::Split45) {
            p1 = trs.edges[top].p2;
            p2 = trs.edges[bottom].p1;
        } else {
            p1 = trs.edges[top].p1;
            p2 = trs.edges[bottom].p2;
        }
        int midPoint = allocPoint(Point2I(pos.x + squareHalfSize, pos.y + squareHalfSize), p1, p2);

        int nextLevel = n->level - 1;

        subdivideEdge(top, Point2I(pos.x + squareHalfSize, pos.y + squareOneSize));
        subdivideEdge(right, Point2I(pos.x + squareOneSize, pos.y + squareHalfSize));
        subdivideEdge(bottom, Point2I(pos.x + squareHalfSize, pos.y));
        subdivideEdge(left, Point2I(pos.x, pos.y + squareHalfSize));

        int firstCrossEdge = allocEdges(4);

        trs.edges[firstCrossEdge].p1 = trs.edges[top].mp;
        trs.edges[firstCrossEdge].p2 = midPoint;
        trs.edges[firstCrossEdge + 1].p1 = midPoint;
        trs.edges[firstCrossEdge + 1].p2 = trs.edges[right].mp;
        trs.edges[firstCrossEdge + 2].p1 = midPoint;
        trs.edges[firstCrossEdge + 2].p2 = trs.edges[bottom].mp;
        trs.edges[firstCrossEdge + 3].p1 = trs.edges[left].mp;
        trs.edges[firstCrossEdge + 3].p2 = midPoint;

        for (auto i : std::views::iota(0, 4)) {
            n[i].level = nextLevel;
            n[i].clipFlags = clipFlags;
            n[i].lightMask = lightMask;
        }

        n[0].pos = pos;
        n[0].top = firstCrossEdge + 3;
        n[0].right = firstCrossEdge + 2;
        n[0].bottom = trs.edges[bottom].firstSubEdge;
        n[0].left = trs.edges[left].firstSubEdge + 1;

        n[1].pos.set(pos.x + squareHalfSize, pos.y);
        n[1].top = firstCrossEdge + 1;
        n[1].right = trs.edges[right].firstSubEdge + 1;
        n[1].bottom = trs.edges[bottom].firstSubEdge + 1;
        n[1].left = firstCrossEdge + 2;

        n[2].pos.set(pos.x + squareHalfSize, pos.y + squareHalfSize);
        n[2].top = trs.edges[top].firstSubEdge + 1;
        n[2].right = trs.edges[right].firstSubEdge;
        n[2].bottom = firstCrossEdge + 1;
        n[2].left = firstCrossEdge;

        n[3].pos.set(pos.x, pos.y + squareHalfSize);
        n[3].top = trs.edges[top].firstSubEdge;
        n[3].right = firstCrossEdge;
        n[3].bottom = firstCrossEdge + 3;
        n[3].left = trs.edges[left].firstSubEdge;
        curStackSize += 3;
    }
}

float NativeTerrainRender::getWaterHeight(Point2F pos, int pointIndex) {
    float waveHeight = 0;
    Point3F normal(0, 0, 1);
    float sinAngle, cosAngle;

    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return i < trs.waveCount;
                  })) {
        Point4F& wave = trs.waveParameters[i];
        float temp = pos.x * wave.x + pos.y * wave.y + wave.z;
        m_sincos(temp, &sinAngle, &cosAngle);
        waveHeight += wave.w * sinAngle;
        normal.x += wave.w * wave.x * cosAngle;
        normal.y += wave.w * wave.y * cosAngle;
    }
    normal.normalize();

    Point3F vec(pos.x - trs.camPos.x, pos.y - trs.camPos.y, waveHeight - trs.camPos.z);
    vec += normal * (-2 * m_dot(vec, normal));
    vec.normalize(0.5);
    trs.envTexCoords[pointIndex].set(vec.x + 0.5, vec.y + 0.5);

    return waveHeight;
}

void NativeTerrainRender::processWaterBlock(Point2F extent, float height) {
    WaterStackNode stack[MaxWaterStackSize];

    int p1, p2, p3, p4;

    p1 = allocWaterPoint(Point2F(0, extent.y), 0, 0);
    p2 = allocWaterPoint(Point2F(extent.x, extent.y), 0, 0);
    p3 = allocWaterPoint(Point2F(extent.x, 0), 0, 0);
    p4 = allocWaterPoint(Point2F(0, 0), 0, 0);

    int e0 = allocEdges(4);
    trs.edges[e0].p1 = p1;
    trs.edges[e0].p2 = p2;
    trs.edges[e0 + 1].p1 = p2;
    trs.edges[e0 + 1].p2 = p3;
    trs.edges[e0 + 2].p1 = p4;
    trs.edges[e0 + 2].p2 = p3;
    trs.edges[e0 + 3].p1 = p1;
    trs.edges[e0 + 3].p2 = p4;

    stack[0].position.set(0, 0);
    stack[0].extent = extent;
    stack[0].clipFlags = 0xFF;
    stack[0].top = e0;
    stack[0].right = e0 + 1;
    stack[0].bottom = e0 + 2;
    stack[0].left = e0 + 3;

    int curStackSize = 1;
    trs.cameraSquare = -1;

    float ps = screenSize / trs.camera->getPixelScale();
    float distanceScale = trs.camera->projectionDistance(ps, 1);
    Point3F minPoint, maxPoint;

    while (curStackSize) {
        WaterStackNode* n = stack + curStackSize - 1;
        minPoint.set(n->position.x, n->position.y, -height);
        maxPoint.set(n->position.x + n->extent.x, n->position.y + n->extent.y, height);

        trs.currentGrowFactor = 0;
        int clipFlags = n->clipFlags;
        if (clipFlags) {
            clipFlags = TestSquareVisibility(minPoint, maxPoint, clipFlags, trs.minWaterSquareSize);
            if (clipFlags == -1) {
                curStackSize--;
                continue;
            }
        }
        Point2F pos = n->position, ext = n->extent;
        int top = n->top, right = n->right, bottom = n->bottom, left = n->left;

        if (n->extent.x >= n->extent.y * 2) {

            float midX = pos.x + ext.x * .5;
            subdivideWaterEdge(top, Point2F(midX, pos.y + ext.y));
            subdivideWaterEdge(bottom, Point2F(midX, pos.y));

            int crossEdge = allocEdges(1);
            trs.edges[crossEdge].p1 = trs.edges[top].mp;
            trs.edges[crossEdge].p2 = trs.edges[bottom].mp;

            n[0].position = pos;
            n[0].extent.set(midX - pos.x, ext.y);

            n[0].top = trs.edges[top].firstSubEdge;
            n[0].right = crossEdge;
            n[0].bottom = trs.edges[bottom].firstSubEdge;
            n[0].left = left;
            n[0].clipFlags = clipFlags;

            n[1].position.set(midX, pos.y);
            n[1].extent.set(pos.x + ext.x - midX, ext.y);

            n[1].top = trs.edges[top].firstSubEdge + 1;
            n[1].right = right;
            n[1].bottom = trs.edges[bottom].firstSubEdge + 1;
            n[1].left = crossEdge;
            n[1].clipFlags = clipFlags;

            curStackSize++;
            continue;
        }
        if (n->extent.y >= n->extent.x * 2) {

            float midY = pos.y + ext.y * .5;
            subdivideWaterEdge(left, Point2F(pos.x, midY));
            subdivideWaterEdge(right, Point2F(pos.x + ext.x, midY));

            int crossEdge = allocEdges(1);
            trs.edges[crossEdge].p1 = trs.edges[left].mp;
            trs.edges[crossEdge].p2 = trs.edges[right].mp;

            n[0].position = pos;
            n[0].extent.set(ext.x, midY - pos.y);

            n[0].top = crossEdge;
            n[0].right = trs.edges[right].firstSubEdge + 1;
            n[0].bottom = bottom;
            n[0].left = trs.edges[left].firstSubEdge + 1;
            n[0].clipFlags = clipFlags;

            n[1].position.set(pos.x, midY);
            n[1].extent.set(ext.x, pos.y + ext.y - midY);

            n[1].top = top;
            n[1].right = trs.edges[right].firstSubEdge;
            n[1].bottom = crossEdge;
            n[1].left = trs.edges[left].firstSubEdge;
            n[1].clipFlags = clipFlags;

            curStackSize++;
            continue;
        }

        float blockDistance = getSquareDistance(minPoint, maxPoint);

        if (trs.edgeCount > ScanEdgeLimit || trs.pointCount > ScanPointLimit ||
            ext.x <= trs.minWaterSquareSize || ext.y <= trs.minWaterSquareSize) {

            if (blockDistance < 0.01)
                trs.cameraSquare = trs.squareCount;

            emitWaterSquare(n);
            curStackSize--;
            continue;
        }

        float squareSize = ext.x > ext.y ? ext.x : ext.y;
        float detailDistance = distanceScale * squareSize;
        if (blockDistance >= detailDistance) {
            clampEdge(top);
            clampEdge(right);
            clampEdge(bottom);
            clampEdge(left);

            emitWaterSquare(n);
            curStackSize--;
            continue;
        }

        float clampDistance = detailDistance * 0.4;
        if (blockDistance > clampDistance)
            trs.currentGrowFactor = (blockDistance - clampDistance) / (0.6 * detailDistance);

        float midX = pos.x + ext.x * 0.5;
        float midY = pos.y + ext.y * 0.5;

        int topPoint = trs.edges[top].p1;
        int bottomPoint = trs.edges[bottom].p2;

        int midPoint = allocWaterPoint(Point2F(midX, midY), topPoint, bottomPoint);

        subdivideWaterEdge(top, Point2F(midX, pos.y + ext.y));
        subdivideWaterEdge(right, Point2F(pos.x + ext.x, midY));
        subdivideWaterEdge(bottom, Point2F(midX, pos.y));
        subdivideWaterEdge(left, Point2F(pos.x, midY));

        int firstCrossEdge = allocEdges(4);

        trs.edges[firstCrossEdge].p1 = trs.edges[top].mp;
        trs.edges[firstCrossEdge].p2 = midPoint;
        trs.edges[firstCrossEdge + 1].p1 = midPoint;
        trs.edges[firstCrossEdge + 1].p2 = trs.edges[right].mp;
        trs.edges[firstCrossEdge + 2].p1 = midPoint;
        trs.edges[firstCrossEdge + 2].p2 = trs.edges[bottom].mp;
        trs.edges[firstCrossEdge + 3].p1 = trs.edges[left].mp;
        trs.edges[firstCrossEdge + 3].p2 = midPoint;

        n[0].position = pos;
        n[0].extent.set(midX - pos.x, midY - pos.y);
        n[0].top = firstCrossEdge + 3;
        n[0].right = firstCrossEdge + 2;
        n[0].bottom = trs.edges[bottom].firstSubEdge;
        n[0].left = trs.edges[left].firstSubEdge + 1;

        n[1].position.set(midX, pos.y);
        n[1].extent.set(pos.x + ext.x - midX, midY - pos.y);
        n[1].top = firstCrossEdge + 1;
        n[1].right = trs.edges[right].firstSubEdge + 1;
        n[1].bottom = trs.edges[bottom].firstSubEdge + 1;
        n[1].left = firstCrossEdge + 2;

        n[2].position.set(midX, midY);
        n[2].extent.set(pos.x + ext.x - midX, pos.y + ext.y - midY);
        n[2].top = trs.edges[top].firstSubEdge + 1;
        n[2].right = trs.edges[right].firstSubEdge;
        n[2].bottom = firstCrossEdge + 1;
        n[2].left = firstCrossEdge;

        n[3].position.set(pos.x, midY);
        n[3].extent.set(midX - pos.x, pos.y + ext.y - midY);
        n[3].top = trs.edges[top].firstSubEdge;
        n[3].right = firstCrossEdge;
        n[3].bottom = firstCrossEdge + 3;
        n[3].left = trs.edges[left].firstSubEdge;

        for (auto i : std::views::iota(0, 4))
            n[i].clipFlags = clipFlags;

        curStackSize += 3;
    }
}
