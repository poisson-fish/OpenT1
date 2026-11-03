// Port of LS_Math.cpp (Dynamix, 1996). See terrain-authoring-port.json.
void NativeLandscape::add(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "ADD: nothing on stack to add");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    for (auto& value : stack[0]->values)
        value += _n;
}

void NativeLandscape::sub(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "SUB: nothing on stack to subtract");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    for (auto& value : stack[0]->values)
        value -= _n;
}

void NativeLandscape::mul(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "MUL: nothing on stack to multiply");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    for (auto& value : stack[0]->values)
        value *= _n;
}

void NativeLandscape::div(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "DIV: nothing on stack to divide");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    for (auto& value : stack[0]->values)
        value /= _n;
}

void NativeLandscape::exp(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "EXP: nothing on stack to exp");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    for (auto& value : stack[0]->values)
        value = pow(value, _n);
}

void NativeLandscape::neg() {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "NEG: nothing on stack to neg");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    for (auto& value : stack[0]->values)
        value = -value;
}

void NativeLandscape::clr(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "NEG: nothing on stack to clear");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = square(stack[0]->size);

    memset(stack[0]->real, _n, length * sizeof(float));
}

void NativeLandscape::normalize(float _min, float _max) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "NORMALIZE: nothing on stack to normalize");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float* realPtr = stack[0]->real;

    int length = square(stack[0]->size);
    float min = FLT_MAX;
    float max = -FLT_MAX;

    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return i < length;
                  })) {
        min = fmin(min, realPtr[i]);
        max = fmax(max, realPtr[i]);
    }

    if (!(max - min))
        return;

    float ratio = (_max - _min) / (max - min);

    for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                      return i < length;
                  }))
        realPtr[i] = ratio * (realPtr[i] - min) + _min;
}

void NativeLandscape::addStack(int _n) {
    if (stack.size() < std::size_t(_n + 1)) {
        std::snprintf(mExecString, sizeof(mExecString), "ADD_STACK: stack too small for operation");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    if (stack[0]->size != stack[_n]->size) {
        std::snprintf(mExecString, sizeof(mExecString), "ADD_STACK: can't add different sized matrices");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int size = stack[0]->size;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  }))
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      }))
            stack[0]->real[y * size + x] += stack[_n]->real[y * size + x];

    stack[_n].reset();
    stack.erase(stack.begin() + _n);
}

void NativeLandscape::subStack(int _n) {
    if (stack.size() < std::size_t(_n + 1)) {
        std::snprintf(mExecString, sizeof(mExecString), "SUB_STACK: stack too small for operation");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    if (stack[0]->size != stack[_n]->size) {
        std::snprintf(mExecString, sizeof(mExecString), "SUB_STACK: can't subtract different sized matrices");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int size = stack[0]->size;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  }))
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      }))
            stack[0]->real[y * size + x] =
                stack[_n]->real[y * size + x] - stack[0]->real[y * size + x];

    stack[_n].reset();
    stack.erase(stack.begin() + _n);
}

float normalProbability(float _mean, float _sdev, float _val) {
    _val = (_val - _mean) / _sdev;
    if (_val < 0.0)
        _val = -_val;
    float t = 1 / (1 + 0.3326 * _val);
    float r = (exp(-_val * _val * 0.5f)) * (0.398942280);
    return (0.5 - r * t * (0.4361836 - 0.1201676 * t + 0.937298 * t * t));
}

[[maybe_unused]] float
normalProbability(float _min, float _max, float _mean, float _dev, float _val) {
    float x;

    if (_val == _mean)
        return (0.0);
    else if (_val < _min)
        return (0.5);
    else if (_val > _max)
        return (0.5);
    else if (_val < _mean)
        x = (_val - _mean) / (_mean - _min);
    else if (_val > _mean)
        x = (_val - _mean) / (_max - _mean);
    else
        return (0.5);

    return (normalProbability(0.0, _dev, x));
}

// Port of LS_Cmplx.cpp (Dynamix, 1996). See terrain-authoring-port.json.
void NativeLandscape::overlay(int _n, float _thresh) {
    if (stack.size() < std::size_t(_n + 1)) {
        std::snprintf(mExecString, sizeof(mExecString), "OVERLAY: Not enough items on the stack to overlay");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int size = stack[0]->size;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  }))
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      }))
            if (stack[0]->real[y * size + x] <= _thresh)
                stack[0]->real[y * size + x] = stack[_n]->real[y * size + x];

    stack[_n].reset();
    stack.erase(stack.begin() + _n);
}

void NativeLandscape::blend(int _n, int _dist) {
    if (stack.size() < std::size_t(_n + 1)) {
        std::snprintf(mExecString, sizeof(mExecString), "BLEND: not enough items on the stack to blend");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int y, i;
    float avPix;
    int avTot, avIdx;

    int size = stack[0]->size;

    for (y = 0; y < size * size; y += size) {
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      })) {
            if (stack[_n]->real[y + x]) {
                for (auto r : std::views::iota(-_dist) | std::views::take_while([&](int r) {
                                  return r <= _dist;
                              })) {
                    for (auto c : std::views::iota(-_dist) | std::views::take_while([&](int c) {
                                      return c <= _dist;
                                  })) {
                        avPix = avTot = 0;
                        if (x + c >= 0 && x + c < size && y / size + r >= 0 &&
                            y / size + r < size && !stack[_n]->real[x + c + y + r * size]) {
                            for (i = -size; i <= size; i += size) {
                                for (auto j :
                                     std::views::iota(-1) | std::views::take_while([&](int j) {
                                         return j <= 1;
                                     })) {
                                    avIdx = x + c + j + y + r * size + i;
                                    if ((avIdx >= 0) && (avIdx < size * size)) {
                                        if (!stack[_n]->real[avIdx]) {
                                            avPix += stack[0]->real[avIdx];
                                            avTot++;
                                        }
                                    }
                                }
                            }
                            if (avTot) {
                                avPix = avPix / avTot;

                                stack[0]->real[x + c + y + r * size] =
                                    avPix - (((avPix - stack[_n]->real[x + y]) *
                                              ((float)(2 * _dist) + 1 - abs(r) - abs(c))) /
                                             ((float)(2 * _dist) + 1));
                            }
                        }
                    }
                }
                stack[0]->real[y + x] = stack[_n]->real[y + x];
            }
        }
    }
}

void NativeLandscape::flipX() {
    int width = stack[0]->size;

    if (width < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "FLIPX: Nothing on the stack");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float* realPtr = stack[0]->real;

    for (int y = 0; y < square(width); y += width)
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < (width >> 1);
                      })) {
            float tmp = realPtr[y + x];
            realPtr[y + x] = realPtr[y + width - 1 - x];
            realPtr[y + width - 1 - x] = tmp;
        }
}

void NativeLandscape::flipY() {
    int width = stack[0]->size;

    if (width < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "FLIPY: Nothing on the stack");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float* realPtr = stack[0]->real;

    for (int y = 0; y < (square(width) >> 1); y += width)
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < width;
                      })) {
            float tmp = realPtr[y + x];
            realPtr[y + x] = realPtr[square(width) - y - width + x];
            realPtr[square(width) - width - y + x] = tmp;
        }
}

// Port of LS_Fill.cpp (Dynamix, 1996). See terrain-authoring-port.json.
#define El(vv, xq, yq) vv[((yq) * size) + xq]
#define minf(x, y) (((x) < (y)) ? (x) : (y))

void NativeLandscape::fillNormal(float _height) {
    if (stack.size() < 2) {
        std::snprintf(mExecString, sizeof(mExecString), "FILL_NORMAL: not enough items on the stack to fillNormal (need 2)");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    if (stack[0]->size != stack[1]->size) {
        std::snprintf(mExecString, sizeof(mExecString), "FILL_NORMAL: different sized matrices");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float radius = 0;
    float phase;

    int size = stack[0]->size;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size >> 1;
                  })) {
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size >> 1;
                      })) {

            if ((x | y))
                radius = float(pow((double)(x * x + y * y), (double)-_height) * rand.getNormal());
            phase = float(M_2PI * rand.getFloat());

            El(stack[1]->real, x, y) = float(radius * cos(phase));

            El(stack[0]->real, x, y) = float(radius * sin(phase));

            if ((x | y))
                radius = float(pow((double)(x * x + y * y), (double)-_height) * rand.getNormal());
            phase = float(M_2PI * rand.getFloat());
            El(stack[1]->real, size - x - 1, size - y - 1) = float(radius * cos(phase));
            El(stack[0]->real, size - x - 1, size - y - 1) = float(radius * sin(phase));

            if ((x | y))
                radius = float(pow((double)(x * x + y * y), (double)-_height) * rand.getNormal());
            phase = float(M_2PI * rand.getFloat());
            El(stack[1]->real, x, size - y - 1) = float(radius * cos(phase));
            El(stack[0]->real, x, size - y - 1) = float(radius * sin(phase));

            if ((x | y))
                radius = float(pow((double)(x * x + y * y), (double)-_height) * rand.getNormal());
            phase = float(M_2PI * rand.getFloat());
            El(stack[1]->real, size - x - 1, y) = float(radius * cos(phase));
            El(stack[0]->real, size - x - 1, y) = float(radius * sin(phase));
        }
    }
}

#if 0
void NativeLandscape::fillNormalHalf( float _height )
{
   int      rankmax;
   float    radius=0;
   float    phase, rcos, rsin;

   rankmax = minf( (width / 2.0 - 0.5),(height / 2.0 - 0.5) );

   for (auto y : std::views::iota(0) | std::views::take_while([&](int y) { return y<=rankmax; }))
   {
      for (auto x : std::views::iota(0) | std::views::take_while([&](int x) { return x<=y; }))
      {
         if ( (x|y) )
            radius = _height * (1.0/(x*x+y*y)) * rand.getNormal();

         phase = M_2PI * rand.getFloat();
         rcos = radius * cos(phase);
         rsin = radius * sin(phase);

         El(stack[0]->real, x, y) = rcos;
         El(stack[0]->imag, x, y) = rsin;
         El(stack[0]->real, width-x-1, height-y-1) = rcos;
         El(stack[0]->imag, width-x-1, height-y-1) = rsin;

         phase = M_2PI * rand.getFloat();
         rcos = radius * cos(phase);
         rsin = radius * sin(phase);

         El(stack[0]->real, x, height-y-1) = rcos;
         El(stack[0]->imag, x, height-y-1) = rsin;
         El(stack[0]->real, width-x-1, y)  = rcos;
         El(stack[0]->imag, width-x-1, y)  = rsin;
      }
   }
}
#endif

// Port of LS_terrain.cpp (Dynamix, 1996). See terrain-authoring-port.json.
void NativeLandscape::terrain(int size, float _h) {
    stack.push_front(std::make_unique<Matrix>(size));
    stack.push_front(std::make_unique<Matrix>(size));

    fillNormal(_h);
    fft(1);

    stack.pop_front();
}

struct Plasma {
    float* data;
    int width;
    float deviation;
    Random* rand;

    float& pixel(int x, int y);
    float avrgColor(float c1, float c2);
    float avrgX(int x1, int ax, int x2, int y);
    float avrgY(int x, int y1, int ay, int y2);
    void recurse(int minX, int minY, int maxX, int maxY);
};

inline float& Plasma::pixel(int x, int y) {
    return data[y * width + x];
}

inline float Plasma::avrgColor(float c1, float c2) {
    float offset = deviation * (rand->getFloat() - 0.5f);
    return (c1 + c2) * 0.5 + offset;
}

inline float Plasma::avrgX(int x1, int ax, int x2, int y) {
    float& p = pixel(ax, y);
    return p = (p != 0.0f) ? p : avrgColor(pixel(x1, y), pixel(x2, y));
}

inline float Plasma::avrgY(int x, int y1, int ay, int y2) {
    float& p = pixel(x, ay);
    return p = (p != 0.0f) ? p : avrgColor(pixel(x, y1), pixel(x, y2));
}

void Plasma::recurse(int minX, int minY, int maxX, int maxY) {
    int avgX = (minX + maxX) >> 1;
    int avgY = (minY + maxY) >> 1;

    float ac = 0.0f;
    ac += avrgX(minX, avgX, maxX, minY);
    ac += avrgX(minX, avgX, maxX, maxY);
    ac += avrgY(minX, minY, avgY, maxY);
    ac += avrgY(maxX, minY, avgY, maxY);
    pixel(avgX, avgY) = ac * 0.25;

    if ((maxX - minX) <= 1 || (maxY - minY) <= 1)
        return;

    recurse(minX, minY, avgX, avgY);
    recurse(avgX, minY, maxX, avgY);
    recurse(avgX, avgY, maxX, maxY);
    recurse(minX, avgY, avgX, maxY);
}

void NativeLandscape::plasma(int size, float deviation) {
    stack.push_front(std::make_unique<Matrix>(size));
    clr(0.0f);

    Plasma pp;
    pp.width = stack[0]->size;
    pp.data = stack[0]->real;
    pp.deviation = deviation;
    pp.rand = &rand;

    int minX = 0, maxX = stack[0]->size - 1;
    int minY = 0, maxY = stack[0]->size - 1;

    pp.pixel(minX, minY) = pp.avrgColor(-deviation, deviation);
    pp.pixel(maxX, minY) = pp.avrgColor(-deviation, deviation);
    pp.pixel(maxX, maxY) = pp.avrgColor(-deviation, deviation);
    pp.pixel(minX, maxY) = pp.avrgColor(-deviation, deviation);
    pp.recurse(minX, minY, maxX, maxY);
}

// Port of LS_Fltr.cpp (Dynamix, 1996). See terrain-authoring-port.json.
#define GOOD_TRIG

#define SQRT2 2 * 0.70710678118654752440084436210484
static void fht(REAL* fz, int size);

#define maxf(x, y) (((x) > (y)) ? (x) : (y))
#define absf(x) ((x) > 0.0 ? (x) : (-(x)))

#define modf(x, y) (((x) + (y)) % (y))

#define El(vv, xq, yq) vv[((yq) * size) + xq]
#define Elmod(v, x, y)                                                                             \
    v[(((y) >= 0) ? (y) % size : (((y) % size) + size) % size) * size +                            \
      (((x) >= 0) ? (x) % size : (((x) % size) + size) % size)]
#define Elclip(v, x, y)                                                                            \
    v[(((y) < 0 ? 0 : (((y) >= size) ? (size - 1) : (y))) * size) +                                \
      ((x) < 0 ? 0 : (((x) >= size) ? (size - 1) : (x)))]

void NativeLandscape::wrap() {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "WRAP: nothing on stack to wrap");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    bool ok;
    int size = stack[0]->size;
    float* line;
    std::vector<float> lineBuffer(size);
    float hMax, delta, n0, n1;

    const float factor = 0.5;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  })) {

        line = &(stack[0]->real[y * size]);

        hMax = -99999.0;
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size - 1;
                      })) {
            delta = absf(line[x + 1] - line[x]);
            hMax = maxf(hMax, delta);
        }

        n0 = size - 1;
        n1 = 0;

        while (n1 < size - 1) {

            ok = true;
            for (int x = n0; x != n1; x = modf(x + 1, size)) {
                delta = absf(line[modf(x + 1, size)] - line[x]);
                if (delta > factor * hMax) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                break;

            for (int x = n0; x != n1; x = modf(x + 1, size))
                lineBuffer[x] = (line[modf(x - 1, size)] + line[x] + line[modf(x + 1, size)]) / 3.0;

            for (int x = n0; x != n1; x = modf(x + 1, size))
                line[x] = lineBuffer[x];

            n0--;
            n1++;
        }
    }

    for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                      return x < size;
                  })) {
        line = &(stack[0]->real[x]);
        hMax = -99999.0;
        for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                          return y < size - 1;
                      })) {
            delta = absf(line[(y + 1) * size] - line[y * size]);
            hMax = maxf(hMax, delta);
        }

        n0 = size - 1;
        n1 = 0;
        while (n1 < size - 1) {
            ok = true;
            for (int y = n0; y != n1; y = modf(y + 1, size)) {
                delta = absf(line[modf(y + 1, size) * size] - line[y * size]);
                if (delta > factor * hMax) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                break;

            for (int y = n0; y != n1; y = modf(y + 1, size))
                lineBuffer[y] = (line[modf(y - 1, size) * size] + line[y * size] +
                                 line[modf(y + 1, size) * size]) /
                                3.0;
            for (int y = n0; y != n1; y = modf(y + 1, size))
                line[y * size] = lineBuffer[y];

            n0--;
            n1++;
        }
    }
}

static std::unique_ptr<Matrix> getMatrix(const Matrix* source, int x, int y, int size) {
    int sourceIndex;
    auto m = std::make_unique<Matrix>(size);

    for (auto v : std::views::iota(0) | std::views::take_while([&](int v) {
                      return v < size;
                  }))
        for (auto u : std::views::iota(0) | std::views::take_while([&](int u) {
                          return u < size;
                      })) {
            sourceIndex = (y + v) * source->size + (u + x);
            m->real[v * size + u] = source->real[sourceIndex];
        }

    return m;
}

static void placeMatrix(Matrix& target, const Matrix& source, int x, int y) {
    int targetIndex;

    for (auto v : std::views::iota(0) | std::views::take_while([&](int v) {
                      return v < source.size;
                  }))
        for (auto u : std::views::iota(0) | std::views::take_while([&](int u) {
                          return u < source.size;
                      })) {
            targetIndex = (y + v) * target.size + (u + x);
            target.real[targetIndex] = source.real[v * source.size + u];
        }
}

void NativeLandscape::tile() {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "TILE: Nothing on the stack to tile");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    Matrix* m = stack[0].get();
    int tileSize = m->size / 2;
    auto tile0 = getMatrix(m, tileSize, 0, tileSize);
    auto tile1 = getMatrix(m, 0, 0, tileSize);
    auto tile2 = getMatrix(m, 0, tileSize, tileSize);
    auto tile3 = getMatrix(m, tileSize, tileSize, tileSize);

    auto target = std::make_unique<Matrix>(tileSize * 4);

    placeMatrix(*target, *tile3, 0, 0);
    placeMatrix(*target, *tile2, tileSize, 0);
    placeMatrix(*target, *tile3, 2 * tileSize, 0);
    placeMatrix(*target, *tile2, 3 * tileSize, 0);

    placeMatrix(*target, *tile0, 0, tileSize);
    placeMatrix(*target, *tile1, tileSize, tileSize);
    placeMatrix(*target, *tile0, 2 * tileSize, tileSize);
    placeMatrix(*target, *tile1, 3 * tileSize, tileSize);

    placeMatrix(*target, *tile3, 0, 2 * tileSize);
    placeMatrix(*target, *tile2, tileSize, 2 * tileSize);
    placeMatrix(*target, *tile3, 2 * tileSize, 2 * tileSize);
    placeMatrix(*target, *tile2, 3 * tileSize, 2 * tileSize);

    placeMatrix(*target, *tile0, 0, 3 * tileSize);
    placeMatrix(*target, *tile1, tileSize, 3 * tileSize);
    placeMatrix(*target, *tile0, 2 * tileSize, 3 * tileSize);
    placeMatrix(*target, *tile1, 3 * tileSize, 3 * tileSize);

    pop();
    stack.push_front(std::move(target));
}

void NativeLandscape::alphablend(int x, int y) {

    if (stack.size() < 3) {
        std::snprintf(mExecString, sizeof(mExecString), "ALPHA_BLEND: not enough items on the stack (need 3)");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    if (stack[0]->size != stack[1]->size) {
        std::snprintf(mExecString, sizeof(mExecString), "ALPHA_BLEND: alpha chan and overlay are different sizes");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    Matrix* alpha = stack[0].get();
    Matrix* m1 = stack[1].get();
    Matrix* m0 = stack[2].get();

    float* alphaReal = alpha->real;
    float* m0Real = m0->real;
    float* m1Real = m1->real;

    int size1 = m1->size, size0 = m0->size;
    int x0, y0;
    float va, v1;

    for (auto y1 : std::views::iota(0) | std::views::take_while([&](int y1) {
                       return y1 < size1;
                   }))
        for (auto x1 : std::views::iota(0) | std::views::take_while([&](int x1) {
                           return x1 < size1;
                       })) {

            x0 = x1 + x;
            y0 = y1 + y;

            if (x0 >= 0 && y0 >= 0 && x0 < size0 && y0 < size0) {
                va = alphaReal[y1 * size1 + x1];
                v1 = m1Real[y1 * size1 + x1];
                float& rF = m0Real[y0 * size0 + x0];

                rF = va * v1 + (1.0 - va) * rF;
            }
        }

    pop();
    pop();
}

const float convWeights[] = {
    0.25, 0.75, 1.00, 0.75, 0.25, 0.50, 2.00, 3.00, 2.00, 0.75, 1.00, 3.00, 4.00,
    3.00, 1.00, 0.75, 2.00, 3.00, 2.00, 0.75, 0.25, 0.75, 1.00, 0.75, 0.25,
};
const int convWeightsSize = sizeof(convWeights) / sizeof(float);

void NativeLandscape::smooth(float epsilon, float exponent) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "SMOOTH: Not enough items on the stack to blend");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int xc, yc, i, cDelta;
    float targetVal, delta, sign;

    Matrix* sourceM = stack[0].get();
    float* source = sourceM->real;
    int size = sourceM->size;

    auto targetM = std::make_unique<Matrix>(size);
    float* target = targetM->real;

    const float invEpsilon = epsilon != 0.0 ? 1.0f / epsilon : 0.0f;

    int convolutionSize = sqrt((double)convWeightsSize);

    cDelta = (convolutionSize - 1) / 2;

    float totalWeight = 0;
    for (const auto weight : convWeights)
        totalWeight += weight;

    float invTotalWeight = 1.0 / totalWeight;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  }))
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      })) {

            i = y * size + x;
            targetVal = 0.0;

            const float* conWptr = convWeights;
            int vUpper = y + cDelta;
            int uUpper = x + cDelta;
            int vLower = y - cDelta;
            int uLower = x - cDelta;
            if (vUpper < size && uUpper < size && vLower >= 0 && uLower >= 0) {

                float* fPtr = source + vLower * size + uLower;
                int rowEndToRowStart = size - (vUpper - vLower) - 1;
                for ([[maybe_unused]] auto v :
                     std::views::iota(vLower) | std::views::take_while([&](int v) {
                         return v <= vUpper;
                     })) {

                    for ([[maybe_unused]] auto u :
                         std::views::iota(uLower) | std::views::take_while([&](int u) {
                             return u <= uUpper;
                         }))
                        targetVal += *conWptr++ * *fPtr++;
                    fPtr += rowEndToRowStart;
                }
            } else {

                for (auto v : std::views::iota(vLower) | std::views::take_while([&](int v) {
                                  return v <= vUpper;
                              }))
                    for (auto u : std::views::iota(uLower) | std::views::take_while([&](int u) {
                                      return u <= uUpper;
                                  })) {

                        xc = (u + size) % size;
                        yc = (v + size) % size;

                        targetVal += *conWptr * source[yc * size + xc];
                        conWptr++;
                    }
            }

            targetVal *= invTotalWeight;

            delta = targetVal - source[i];
            sign = delta >= 0.0 ? 1.0 : -1.0;
            delta *= sign;

            if (delta >= epsilon)

                target[i] = targetVal - sign * epsilon;
            else {

                delta *= invEpsilon;

                delta = pow(delta, exponent);

                delta *= epsilon;

                target[i] = source[i] + (delta * sign);
            }
        }

    pop();
    stack.push_front(std::move(targetM));
}

void NativeLandscape::floor(float flr_start, float flr_depth, float scanDepth) {

    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "FLOOR: Nothing on the stack to floor");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int length = square(stack[0]->size);

    float* realPtr = stack[0]->real;

    if ((flr_depth == 0) || (scanDepth == 0)) {

        for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                          return i < length;
                      }))
            if (realPtr[i] < flr_start)
                realPtr[i] = flr_start;
    } else {
        float scale = (flr_depth) / (scanDepth);
        float scanDepthElev = flr_start - scanDepth;
        float flrDepthElev = flr_start - flr_depth;

        for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                          return i < length;
                      })) {
            if (realPtr[i] < scanDepthElev)
                realPtr[i] = flrDepthElev;
            else if (realPtr[i] < flr_start)
                realPtr[i] = flr_start - (flr_start - realPtr[i]) * scale;
        }
    }
}

void NativeLandscape::ceil(float ceil_start, float ceil_height, float scanHeight) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "CEIL: Nothing on the stack to ceil");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int length = square(stack[0]->size);

    float* realPtr = stack[0]->real;

    if ((ceil_height == 0) || (scanHeight == 0)) {

        for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                          return i < length;
                      }))
            if (realPtr[i] > ceil_start)
                realPtr[i] = ceil_start;
    } else {
        float scale = ceil_height / scanHeight;
        float scanHeightElev = ceil_start + scanHeight;
        float ceilHeightElev = ceil_start + ceil_height;

        for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                          return i < length;
                      })) {
            if (realPtr[i] > scanHeightElev)
                realPtr[i] = ceilHeightElev;
            else if (realPtr[i] > ceil_start)
                realPtr[i] = ceil_start + (realPtr[i] - ceil_start) * scale;
        }
    }
}

void NativeLandscape::rot() {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "ROT: Nothing on the stack to rot");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    if (!stack.size())
        return;

    int size = stack[0]->size;
    auto pMTX = std::make_unique<Matrix>(size);

    for (auto row : std::views::iota(0) | std::views::take_while([&](int row) {
                        return row < size;
                    }))
        for (auto col : std::views::iota(0) | std::views::take_while([&](int col) {
                            return col < size;
                        }))
            pMTX->real[(col + 1) * size - row - 1] = stack[0]->real[row * size + col];

    stack.pop_front();

    stack.push_front(std::move(pMTX));
}

void NativeLandscape::curve(float _thresh, int _iter) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "CURVE: Nothing on the stack to curve");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float thresh2 = _thresh * _thresh;

    int size = stack[0]->size;
    float bitSrc, bitDst, deltaX, deltaY;
    bool changed = true;

    dup();

    while (changed && _iter--) {
        changed = false;
        swap();
        float* topReal = stack[0]->real;
        float* nextReal = stack[1]->real;
        float* topXY = topReal;
        float* nextXY = nextReal;
        for (auto iy : std::views::iota(0) | std::views::take_while([&](int iy) {
                           return iy < size;
                       })) {
            for (auto ix : std::views::iota(0) | std::views::take_while([&](int ix) {
                               return ix < size;
                           })) {
                float bitX, bitY, bitX1, bitY1;
                if (ix == 0 || ix == size - 1 || iy == 0 || iy == size - 1) {
                    bitDst = *topXY;
                    bitX = Elmod(topReal, ix - 1, iy);
                    bitY = Elmod(topReal, ix, iy - 1);
                    bitX1 = Elmod(topReal, ix + 1, iy);
                    bitY1 = Elmod(topReal, ix, iy + 1);
                    deltaX = bitDst - (bitX + bitX1) * 0.5f;
                    deltaY = bitDst - (bitY + bitY1) * 0.5f;

                    bitSrc = deltaX * deltaX + deltaY * deltaY;
                    if (bitSrc > thresh2) {
                        bitSrc = (bitX + bitX1 + bitY + bitY1) * 0.25f;
                        changed = true;
                    } else
                        bitSrc = bitDst;
                    *nextXY = bitSrc;
                } else {

                    bitDst = *topXY;
                    bitX = *(topXY - 1);
                    bitY = *(topXY - size);
                    bitX1 = *(topXY + 1);
                    bitY1 = *(topXY + size);
                    deltaX = bitDst - (bitX + bitX1) * 0.5f;
                    deltaY = bitDst - (bitY + bitY1) * 0.5f;

                    bitSrc = deltaX * deltaX + deltaY * deltaY;
                    if (bitSrc > thresh2) {
                        bitSrc = (bitX + bitX1 + bitY + bitY1) * 0.25f;
                        changed = true;
                    } else
                        bitSrc = bitDst;
                    *nextXY = bitSrc;
                }
                topXY++;
                nextXY++;
            }
        }
    }
    pop();
}

void NativeLandscape::slope(float _thresh, int _iter) {
    if (stack.size() < 2) {
        std::snprintf(mExecString, sizeof(mExecString), "SLOPE: Not enough items on the stack to slop (need 2)");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float thresh2 = _thresh * _thresh;

    int size = stack[0]->size;
    float bitSrc, bitDst;
    bool changed = true;

    dup();

    while (changed && _iter--) {
        changed = false;
        swap();
        float* topReal = stack[0]->real;
        float* nextReal = stack[1]->real;
        float* topXY = topReal;
        float* nextXY = nextReal;
        for (auto iy : std::views::iota(0) | std::views::take_while([&](int iy) {
                           return iy < size;
                       })) {
            for (auto ix : std::views::iota(0) | std::views::take_while([&](int ix) {
                               return ix < size;
                           })) {
                float bitX, bitY, bitX1, bitY1;
                float deltaX, deltaY;
                if (ix == 0 || ix == size - 1 || iy == 0 || iy == size - 1) {
                    bitDst = *topXY;
                    bitX = Elmod(topReal, ix - 1, iy);
                    bitY = Elmod(topReal, ix, iy - 1);
                    deltaX = bitDst - bitX;
                    deltaY = bitDst - bitY;

                    bitSrc = deltaX * deltaX + deltaY * deltaY;
                    if (bitSrc > thresh2) {
                        bitX1 = Elmod(topReal, ix + 1, iy);
                        bitY1 = Elmod(topReal, ix, iy + 1);
                        bitSrc = (bitX + bitX1 + bitY + bitY1) * 0.25f;
                        changed = true;
                    } else
                        bitSrc = bitDst;
                    *nextXY = bitSrc;
                } else {

                    bitDst = *topXY;
                    bitX = *(topXY - 1);
                    bitY = *(topXY - size);
                    deltaX = bitDst - bitX;
                    deltaY = bitDst - bitY;

                    bitSrc = deltaX * deltaX + deltaY * deltaY;
                    if (bitSrc > thresh2) {
                        bitX1 = *(topXY + 1);
                        bitY1 = *(topXY + size);
                        bitSrc = (bitX + bitX1 + bitY + bitY1) * 0.25f;
                        changed = true;
                    } else
                        bitSrc = bitDst;
                    *nextXY = bitSrc;
                }
                topXY++;
                nextXY++;
            }
        }
    }
    pop();
}

const float convWeights2[] = {
    0.50, 1.00, 1.00, 1.00, 0.50, 1.00, 2.00, 2.00, 2.00, 1.00, 1.00, 2.00, 1.00,
    2.00, 1.00, 1.00, 2.00, 2.00, 2.00, 1.00, 0.50, 1.00, 1.00, 1.00, 0.50,
};
const int convWeightsSize2 = sizeof(convWeights2) / sizeof(float);

void NativeLandscape::shaveArea(float epsilon, float exponent, float avgWeight) {
    if (stack.size() < 2) {
        std::snprintf(mExecString, sizeof(mExecString), "SHAVE_AREA: Stack must contain shave filter and target terrain.");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    swap();
    int size = stack[0]->size;
    float* source = stack[0]->real;
    float* shaver = stack[1]->real;

    int avgCnt = 0;
    float avg = 0.0f;
    for (const auto index : std::views::iota(0, size * size)) {
        if (shaver[index] != 0.0f) {
            avg += source[index];
            ++avgCnt;
        }
    }
    if (avgCnt != 0)
        avg /= (float)avgCnt;

    int xc, yc, i, cDelta;
    float targetVal, delta, sign;

    const float invEpsilon = epsilon != 0.0 ? 1.0f / epsilon : 0.0f;

    int convolutionSize = sqrt((double)convWeightsSize2);

    cDelta = (convolutionSize - 1) / 2;

    float totalWeight = 0;
    for (const auto weight : convWeights2)
        totalWeight += weight;

    float invTotalWeight = 1.0 / totalWeight;

    auto targetM = std::make_unique<Matrix>(size);

    source = stack[0]->real;
    shaver = stack[1]->real;
    float* target = targetM->real;

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  }))
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      })) {
            i = y * size + x;
            if (*shaver++ == 0.0f) {
                target[i] = source[i];
                continue;
            }

            targetVal = 0.0;

            const float* conWptr = convWeights;
            int vUpper = y + cDelta;
            int uUpper = x + cDelta;
            int vLower = y - cDelta;
            int uLower = x - cDelta;
            if (vUpper < size && uUpper < size && vLower >= 0 && uLower >= 0) {

                float* fPtr = source + vLower * size + uLower;
                int rowEndToRowStart = size - (vUpper - vLower) - 1;
                for ([[maybe_unused]] auto v :
                     std::views::iota(vLower) | std::views::take_while([&](int v) {
                         return v <= vUpper;
                     })) {

                    for ([[maybe_unused]] auto u :
                         std::views::iota(uLower) | std::views::take_while([&](int u) {
                             return u <= uUpper;
                         }))
                        targetVal += *conWptr++ * *fPtr++;
                    fPtr += rowEndToRowStart;
                }
            } else {

                for (auto v : std::views::iota(vLower) | std::views::take_while([&](int v) {
                                  return v <= vUpper;
                              }))
                    for (auto u : std::views::iota(uLower) | std::views::take_while([&](int u) {
                                      return u <= uUpper;
                                  })) {

                        xc = (u + size) % size;
                        yc = (v + size) % size;

                        targetVal += *conWptr * source[yc * size + xc];
                        conWptr++;
                    }
            }

            targetVal *= invTotalWeight;
            targetVal = (1 - avgWeight) * targetVal + avgWeight * avg;

            delta = targetVal - source[i];
            sign = delta >= 0.0 ? 1.0 : -1.0;
            delta *= sign;

            if (delta >= epsilon)

                target[i] = targetVal - sign * epsilon;
            else {

                delta *= invEpsilon;

                delta = pow(delta, exponent);

                delta *= epsilon;

                target[i] = source[i] + (delta * sign);
            }
        }

    pop();
    stack.push_front(std::move(targetM));
    swap();
}

void NativeLandscape::clamp(int _detail, float _deviation) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "CLAMP: Nothing on the stack to clamp");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int width = stack[0]->size;
    float* hfBase = stack[0]->real;
    int wmask = width - 1;

    while (--_detail >= 0) {
        Point2I offset(1 << _detail, width << _detail);
        float range = float(1 << _detail) * _deviation;
        int size = (width >> _detail) + 1;

        for (const auto row : std::views::iota(0, std::max(0, size)))
            for (const auto column : std::views::iota(0, std::max(0, size))) {
                const Point2I pos{column, row};
                int index = (((pos.y << _detail) & wmask) * (width)) + ((pos.x << _detail) & wmask);

                Point2I ho(0, 0);
                switch (((pos.x & 1) << 1) | (pos.y & 1)) {
                    case 1:
                        ho.set(0, 1);
                        break;
                    case 2:
                        ho.set(1, 0);
                        break;
                    case 3:
                        ho.y = 1;
                        if ((pos.x ^ pos.y) & 2)
                            ho.x = -1;
                        else
                            ho.x = 1;
                        break;
                }

                if (ho.x || ho.y) {
                    int i1 = ((((pos.y + ho.y) << _detail) & wmask) * (width)) +
                             (((pos.x + ho.x) << _detail) & wmask);
                    int i2 = ((((pos.y - ho.y) << _detail) & wmask) * (width)) +
                             (((pos.x - ho.x) << _detail) & wmask);

                    float target = (hfBase[i1] + hfBase[i2]) * 0.5f;

                    float& here = hfBase[index];
                    if (here > target + range)
                        here = target + range;
                    else if (here < target - range)
                        here = target - range;
                }
            }
    }
}

void NativeLandscape::mask(int _detail, float _scale) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "MASK: Nothing on the stack to mask");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    if (_detail > 0) {
        int width = stack[0]->size;
        float* base = stack[0]->real;
        int dsize = 1 << _detail;
        int dmask = dsize - 1;
        float dscale = (1.0f / dsize) * 2.0f;

        for (const auto row : std::views::iota(0, std::max(0, width)))
            for (const auto column : std::views::iota(0, std::max(0, width))) {
                const Point2I pos{column, row};

                float x = (pos.x & dmask) * dscale - 1.0f;
                x = (x >= 0.0f) ? 1.0f - x : 1.0f + x;
                float y = (pos.y & dmask) * dscale - 1.0f;
                y = (y >= 0.0f) ? 1.0f - y : 1.0f + y;

                base[(pos.y * width) + pos.x] *= x * y * _scale;
            }
    }
}

void NativeLandscape::diff() {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "DIFF: nothing on stack to diff");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int size = stack[0]->size;
    float bitDst;

    dup();

    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < size;
                  }))
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < size;
                      })) {
            bitDst = El(stack[0]->real, x, y);
            El(stack[0]->real, x, y) = sqrt(pow(bitDst - Elmod(stack[0]->real, x - 1, y), 2) +
                                            pow(bitDst - Elmod(stack[0]->real, x, y - 1), 2));
        }

    pop();
}

void NativeLandscape::fft(int _sign) {
    if (stack.size() < 2) {
        std::snprintf(mExecString, sizeof(mExecString), "FFT: Nothing on the stack to fft");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int msize = square(stack[0]->size);
    int half = msize >> 1;
    float* rPi = stack[0]->real;
    float* rPj = &stack[0]->real[msize - 1];
    float* iPi = stack[1]->real;
    float* iPj = &stack[1]->real[msize - 1];

    if (_sign > 0) {
        for (const auto i : std::views::iota(1, std::max(1, half))) {
            double a, b, c, d;
            double q, r, s, t;
            a = rPi[i - 1];
            b = rPj[1 - i];
            q = a + b;
            r = a - b;
            c = iPi[i - 1];
            d = iPj[1 - i];
            s = c + d;
            t = c - d;
            rPi[i - 1] = (q + t) * .5;
            rPj[1 - i] = (q - t) * .5;
            iPi[i - 1] = (s - r) * .5;
            iPj[1 - i] = (s + r) * .5;
        }
        fht(stack[0]->real, msize);
        fht(stack[1]->real, msize);
    } else {
        fht(stack[0]->real, msize);
        fht(stack[1]->real, msize);
        for (const auto i : std::views::iota(1, std::max(1, half))) {
            double a, b, c, d;
            double q, r, s, t;
            a = rPi[i - 1];
            b = rPj[1 - i];
            q = a + b;
            r = a - b;
            c = iPi[i - 1];
            d = iPj[1 - i];
            s = c + d;
            t = c - d;
            iPi[i - 1] = (s + r) * 0.5;
            iPj[1 - i] = (s - r) * 0.5;
            rPi[i - 1] = (q - t) * 0.5;
            rPj[1 - i] = (q + t) * 0.5;
        }
    }
}

void fht(REAL* fz, int size) {
    int k;
    REAL *fi, *fn, *gi;
    TRIG_VARS;

    for (int k2 = 0; auto k1 : std::views::iota(1) | std::views::take_while([&](auto k1) {
                                   return k1 < size;
                               })) {
        REAL a;
        for (int bit = size >> 1; (!((k2 ^= bit) & bit)); bit >>= 1)
            ;
        if (k1 > k2) {
            a = fz[k1];
            fz[k1] = fz[k2];
            fz[k2] = a;
        }
    }
    k = size > 1 ? int(std::bit_width(unsigned(size - 1))) : 0;
    k &= 1;
    if (k == 0) {
        for (fi = fz, fn = fz + size; fi < fn; fi += 4) {
            REAL f0, f1, f2, f3;
            f1 = fi[0] - fi[1];
            f0 = fi[0] + fi[1];
            f3 = fi[2] - fi[3];
            f2 = fi[2] + fi[3];
            fi[2] = (f0 - f2);
            fi[0] = (f0 + f2);
            fi[3] = (f1 - f3);
            fi[1] = (f1 + f3);
        }
    } else {
        for (fi = fz, fn = fz + size, gi = fi + 1; fi < fn; fi += 8, gi += 8) {
            REAL s1, c1, s2, c2, s3, c3, s4, c4, g0, f0, f1, g1, f2, g2, f3, g3;
            c1 = fi[0] - gi[0];
            s1 = fi[0] + gi[0];
            c2 = fi[2] - gi[2];
            s2 = fi[2] + gi[2];
            c3 = fi[4] - gi[4];
            s3 = fi[4] + gi[4];
            c4 = fi[6] - gi[6];
            s4 = fi[6] + gi[6];
            f1 = (s1 - s2);
            f0 = (s1 + s2);
            g1 = (c1 - c2);
            g0 = (c1 + c2);
            f3 = (s3 - s4);
            f2 = (s3 + s4);
            g3 = SQRT2 * c4;
            g2 = SQRT2 * c3;
            fi[4] = f0 - f2;
            fi[0] = f0 + f2;
            fi[6] = f1 - f3;
            fi[2] = f1 + f3;
            gi[4] = g0 - g2;
            gi[0] = g0 + g2;
            gi[6] = g1 - g3;
            gi[2] = g1 + g3;
        }
    }
    if (size < 16)
        return;

    int k4;
    do {
        REAL s1, c1;
        int k1, k2, k3, kx;
        k += 2;
        k1 = 1 << k;
        k2 = k1 << 1;
        k3 = k2 + k1;
        k4 = k2 << 1;
        kx = k1 >> 1;
        fi = fz;
        gi = fi + kx;
        fn = fz + size;
        do {
            REAL g0, f0, f1, g1, f2, g2, f3, g3;
            f1 = fi[0] - fi[k1];
            f0 = fi[0] + fi[k1];
            f3 = fi[k2] - fi[k3];
            f2 = fi[k2] + fi[k3];
            fi[k2] = f0 - f2;
            fi[0] = f0 + f2;
            fi[k3] = f1 - f3;
            fi[k1] = f1 + f3;
            g1 = gi[0] - gi[k1];
            g0 = gi[0] + gi[k1];
            g3 = SQRT2 * gi[k3];
            g2 = SQRT2 * gi[k2];
            gi[k2] = g0 - g2;
            gi[0] = g0 + g2;
            gi[k3] = g1 - g3;
            gi[k1] = g1 + g3;
            gi += k4;
            fi += k4;
        } while (fi < fn);
        TRIG_INIT(k, c1, s1);
        for (auto angleIndex : std::views::iota(1) | std::views::take_while([&](int angleIndex) {
                                   return angleIndex < kx;
                               })) {
            REAL c2, s2;
            TRIG_NEXT(k, c1, s1);
            c2 = c1 * c1 - s1 * s1;
            s2 = 2 * (c1 * s1);
            fn = fz + size;
            fi = fz + angleIndex;
            gi = fz + k1 - angleIndex;
            do {
                REAL a, b, g0, f0, f1, g1, f2, g2, f3, g3;
                b = s2 * fi[k1] - c2 * gi[k1];
                a = c2 * fi[k1] + s2 * gi[k1];
                f1 = fi[0] - a;
                f0 = fi[0] + a;
                g1 = gi[0] - b;
                g0 = gi[0] + b;
                b = s2 * fi[k3] - c2 * gi[k3];
                a = c2 * fi[k3] + s2 * gi[k3];
                f3 = fi[k2] - a;
                f2 = fi[k2] + a;
                g3 = gi[k2] - b;
                g2 = gi[k2] + b;
                b = s1 * f2 - c1 * g3;
                a = c1 * f2 + s1 * g3;
                fi[k2] = f0 - a;
                fi[0] = f0 + a;
                gi[k3] = g1 - b;
                gi[k1] = g1 + b;
                b = c1 * g2 - s1 * f3;
                a = s1 * g2 + c1 * f3;
                gi[k2] = g0 - a;
                gi[0] = g0 + a;
                fi[k3] = f1 - b;
                fi[k1] = f1 + b;
                gi += k4;
                fi += k4;
            } while (fi < fn);
        }
        TRIG_RESET(k, c1, s1);
    } while (k4 < size);
}

void NativeLandscape::clipMin(float minHeight) {
    if (stack[0]->size < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "CLIP_MIN: Nothing on the stack");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    for (auto& value : stack[0]->values)
        if (value < minHeight)
            value = minHeight;
}

void NativeLandscape::clipMax(float maxHeight) {
    if (stack[0]->size < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "CLIP_MAX: Nothing on the stack");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    for (auto& value : stack[0]->values)
        if (value > maxHeight)
            value = maxHeight;
}
