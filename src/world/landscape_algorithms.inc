// Port of LS_Math.cpp (Dynamix, 1996). See terrain-authoring-port.json.
void NativeLandscape::add(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "ADD: nothing on stack to add");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);

    for (int i = 0; i < length; i++)
        stack[0]->real[i] += _n;
}

void NativeLandscape::sub(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "SUB: nothing on stack to subtract");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);

    for (int i = 0; i < length; i++)
        stack[0]->real[i] -= _n;
}

void NativeLandscape::mul(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "MUL: nothing on stack to multiply");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);

    for (int i = 0; i < length; i++)
        stack[0]->real[i] *= _n;
}

void NativeLandscape::div(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "DIV: nothing on stack to divide");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);

    for (int i = 0; i < length; i++)
        stack[0]->real[i] /= _n;
}

void NativeLandscape::exp(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "EXP: nothing on stack to exp");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);

    for (int i = 0; i < length; i++)
        stack[0]->real[i] = pow(stack[0]->real[i], _n);
}

void NativeLandscape::neg() {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "NEG: nothing on stack to neg");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);
    for (int i = 0; i < length; i++)
        stack[0]->real[i] = -stack[0]->real[i];
}

void NativeLandscape::clr(float _n) {
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "NEG: nothing on stack to clear");
        mExecStatus = EXEC_FAILURE;
        return;
    }
    int length = sq(stack[0]->size);

    memset(stack[0]->real, _n, length * sizeof(float));
}

void NativeLandscape::normalize(float _min, float _max) {
    int i;
    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "NORMALIZE: nothing on stack to normalize");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    float* realPtr = stack[0]->real;

    int length = sq(stack[0]->size);
    float min = FLT_MAX;
    float max = -FLT_MAX;

    for (i = 0; i < length; i++) {
        min = fmin(min, realPtr[i]);
        max = fmax(max, realPtr[i]);
    }

    if (!(max - min))
        return;

    float ratio = (_max - _min) / (max - min);

    for (i = 0; i < length; i++)
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

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
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

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
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

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
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

    int x, y, r, c, i, j;
    float avPix;
    int avTot, avIdx;

    int size = stack[0]->size;

    for (y = 0; y < size * size; y += size) {
        for (x = 0; x < size; x++) {
            if (stack[_n]->real[y + x]) {
                for (r = -_dist; r <= _dist; r++) {
                    for (c = -_dist; c <= _dist; c++) {
                        avPix = avTot = 0;
                        if (x + c >= 0 && x + c < size && y / size + r >= 0 &&
                            y / size + r < size && !stack[_n]->real[x + c + y + r * size]) {
                            for (i = -size; i <= size; i += size) {
                                for (j = -1; j <= 1; j++) {
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

    for (int y = 0; y < sq(width); y += width)
        for (int x = 0; x < (width >> 1); x++) {
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

    for (int y = 0; y < (sq(width) >> 1); y += width)
        for (int x = 0; x < width; x++) {
            float tmp = realPtr[y + x];
            realPtr[y + x] = realPtr[sq(width) - y - width + x];
            realPtr[sq(width) - width - y + x] = tmp;
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

    for (int y = 0; y < size >> 1; y++) {
        for (int x = 0; x < size >> 1; x++) {

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

   for ( int y=0; y<=rankmax; y++ )
   {
      for ( int x=0; x<=y; x++ )
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
    Matrix* real = new Matrix(size);
    stack.push_front(real);
    Matrix* imag = new Matrix(size);
    stack.push_front(imag);

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
    Matrix* real = new Matrix(size);
    stack.push_front(real);
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
    float* lineBuffer = new float[size];
    float hMax, delta, n0, n1;
    int y, x;

    const float factor = 0.5;

    for (y = 0; y < size; y++) {

        line = &(stack[0]->real[y * size]);

        hMax = -99999.0;
        for (x = 0; x < size - 1; x++) {
            delta = absf(line[x + 1] - line[x]);
            hMax = maxf(hMax, delta);
        }

        n0 = size - 1;
        n1 = 0;

        while (n1 < size - 1) {

            ok = true;
            for (x = n0; x != n1; x = modf(x + 1, size)) {
                delta = absf(line[modf(x + 1, size)] - line[x]);
                if (delta > factor * hMax) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                break;

            for (x = n0; x != n1; x = modf(x + 1, size))
                lineBuffer[x] = (line[modf(x - 1, size)] + line[x] + line[modf(x + 1, size)]) / 3.0;

            for (x = n0; x != n1; x = modf(x + 1, size))
                line[x] = lineBuffer[x];

            n0--;
            n1++;
        }
    }

    for (x = 0; x < size; x++) {
        line = &(stack[0]->real[x]);
        hMax = -99999.0;
        for (y = 0; y < size - 1; y++) {
            delta = absf(line[(y + 1) * size] - line[y * size]);
            hMax = maxf(hMax, delta);
        }

        n0 = size - 1;
        n1 = 0;
        while (n1 < size - 1) {
            ok = true;
            for (y = n0; y != n1; y = modf(y + 1, size)) {
                delta = absf(line[modf(y + 1, size) * size] - line[y * size]);
                if (delta > factor * hMax) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                break;

            for (y = n0; y != n1; y = modf(y + 1, size))
                lineBuffer[y] = (line[modf(y - 1, size) * size] + line[y * size] +
                                 line[modf(y + 1, size) * size]) /
                                3.0;
            for (y = n0; y != n1; y = modf(y + 1, size))
                line[y * size] = lineBuffer[y];

            n0--;
            n1++;
        }
    }

    delete[] lineBuffer;
}

static Matrix* getMatrix(Matrix* source, int x, int y, int size) {
    int u, v, sourceIndex;
    Matrix* m = new Matrix(size);

    for (v = 0; v < size; v++)
        for (u = 0; u < size; u++) {
            sourceIndex = (y + v) * source->size + (u + x);
            m->real[v * size + u] = source->real[sourceIndex];
        }

    return (m);
}

static void placeMatrix(Matrix* target, Matrix* source, int x, int y) {
    int u, v, targetIndex;

    for (v = 0; v < source->size; v++)
        for (u = 0; u < source->size; u++) {
            targetIndex = (y + v) * target->size + (u + x);
            target->real[targetIndex] = source->real[v * source->size + u];
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
    Matrix* tile0 = getMatrix(m, tileSize, 0, tileSize);
    Matrix* tile1 = getMatrix(m, 0, 0, tileSize);
    Matrix* tile2 = getMatrix(m, 0, tileSize, tileSize);
    Matrix* tile3 = getMatrix(m, tileSize, tileSize, tileSize);

    Matrix* target = new Matrix(tileSize * 4);

    placeMatrix(target, tile3, 0, 0);
    placeMatrix(target, tile2, tileSize, 0);
    placeMatrix(target, tile3, 2 * tileSize, 0);
    placeMatrix(target, tile2, 3 * tileSize, 0);

    placeMatrix(target, tile0, 0, tileSize);
    placeMatrix(target, tile1, tileSize, tileSize);
    placeMatrix(target, tile0, 2 * tileSize, tileSize);
    placeMatrix(target, tile1, 3 * tileSize, tileSize);

    placeMatrix(target, tile3, 0, 2 * tileSize);
    placeMatrix(target, tile2, tileSize, 2 * tileSize);
    placeMatrix(target, tile3, 2 * tileSize, 2 * tileSize);
    placeMatrix(target, tile2, 3 * tileSize, 2 * tileSize);

    placeMatrix(target, tile0, 0, 3 * tileSize);
    placeMatrix(target, tile1, tileSize, 3 * tileSize);
    placeMatrix(target, tile0, 2 * tileSize, 3 * tileSize);
    placeMatrix(target, tile1, 3 * tileSize, 3 * tileSize);

    pop();
    delete tile0;
    delete tile1;
    delete tile2;
    delete tile3;
    stack.push_front(target);
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
    int x0, y0, x1, y1;
    float va, v1;

    for (y1 = 0; y1 < size1; y1++)
        for (x1 = 0; x1 < size1; x1++) {

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
    int x, y, u, v, xc, yc, i, cDelta;
    float targetVal, delta, sign;

    Matrix* sourceM = stack[0].get();
    float* source = sourceM->real;
    int size = sourceM->size;

    Matrix* targetM = new Matrix(size);
    float* target = targetM->real;

    float invEpsilon;
    if (epsilon != 0.0)
        invEpsilon = 1.0f / epsilon;

    int convolutionSize = sqrt((double)convWeightsSize);

    cDelta = (convolutionSize - 1) / 2;

    float totalWeight = 0;
    for (i = 0; i < convWeightsSize; i++)
        totalWeight += convWeights[i];

    float invTotalWeight = 1.0 / totalWeight;

    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {

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
                for (v = vLower; v <= vUpper; v++) {

                    for (u = uLower; u <= uUpper; u++)
                        targetVal += *conWptr++ * *fPtr++;
                    fPtr += rowEndToRowStart;
                }
            } else {

                for (v = vLower; v <= vUpper; v++)
                    for (u = uLower; u <= uUpper; u++) {

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
    stack.push_front(targetM);
}

void NativeLandscape::floor(float flr_start, float flr_depth, float scanDepth) {

    if (stack.size() < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "FLOOR: Nothing on the stack to floor");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int length = sq(stack[0]->size);
    int i;

    float* realPtr = stack[0]->real;

    if ((flr_depth == 0) || (scanDepth == 0)) {

        for (i = 0; i < length; i++)
            if (realPtr[i] < flr_start)
                realPtr[i] = flr_start;
    } else {
        float scale = (flr_depth) / (scanDepth);
        float scanDepthElev = flr_start - scanDepth;
        float flrDepthElev = flr_start - flr_depth;

        for (i = 0; i < length; i++) {
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

    int length = sq(stack[0]->size);
    int i;

    float* realPtr = stack[0]->real;

    if ((ceil_height == 0) || (scanHeight == 0)) {

        for (i = 0; i < length; i++)
            if (realPtr[i] > ceil_start)
                realPtr[i] = ceil_start;
    } else {
        float scale = ceil_height / scanHeight;
        float scanHeightElev = ceil_start + scanHeight;
        float ceilHeightElev = ceil_start + ceil_height;

        for (i = 0; i < length; i++) {
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
    Matrix* pMTX = new Matrix(size);

    for (int row = 0; row < size; row++)
        for (int col = 0; col < size; col++)
            pMTX->real[(col + 1) * size - row - 1] = stack[0]->real[row * size + col];

    stack.pop_front();

    stack.push_front(pMTX);
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
    bool changed = TRUE;

    dup();

    while (changed && _iter--) {
        changed = FALSE;
        swap();
        float* topReal = stack[0]->real;
        float* nextReal = stack[1]->real;
        float* topXY = topReal;
        float* nextXY = nextReal;
        for (int iy = 0; iy < size; iy++) {
            for (int ix = 0; ix < size; ix++) {
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
    bool changed = TRUE;

    dup();

    while (changed && _iter--) {
        changed = FALSE;
        swap();
        float* topReal = stack[0]->real;
        float* nextReal = stack[1]->real;
        float* topXY = topReal;
        float* nextXY = nextReal;
        for (int iy = 0; iy < size; iy++) {
            for (int ix = 0; ix < size; ix++) {
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
    int ix, iy;

    for (iy = 0; iy < size; iy++) {
        for (ix = 0; ix < size; source++, shaver++, ix++) {

            if (*shaver == 0.0f)
                continue;
            avg += *source;
            avgCnt++;
        }
    }
    if (avgCnt != 0)
        avg /= (float)avgCnt;

    int x, y, u, v, xc, yc, i, cDelta;
    float targetVal, delta, sign;

    float invEpsilon;
    if (epsilon != 0.0)
        invEpsilon = 1.0f / epsilon;

    int convolutionSize = sqrt((double)convWeightsSize2);

    cDelta = (convolutionSize - 1) / 2;

    float totalWeight = 0;
    for (i = 0; i < convWeightsSize2; i++)
        totalWeight += convWeights2[i];

    float invTotalWeight = 1.0 / totalWeight;

    Matrix* targetM = new Matrix(size);

    source = stack[0]->real;
    shaver = stack[1]->real;
    float* target = targetM->real;

    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
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
                for (v = vLower; v <= vUpper; v++) {

                    for (u = uLower; u <= uUpper; u++)
                        targetVal += *conWptr++ * *fPtr++;
                    fPtr += rowEndToRowStart;
                }
            } else {

                for (v = vLower; v <= vUpper; v++)
                    for (u = uLower; u <= uUpper; u++) {

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
    stack.push_front(targetM);
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

        Point2I pos;
        for (pos.y = 0; pos.y < size; pos.y++)
            for (pos.x = 0; pos.x < size; pos.x++) {
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

        Point2I pos;
        for (pos.y = 0; pos.y < width; pos.y++)
            for (pos.x = 0; pos.x < width; pos.x++) {

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

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
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

    int msize = sq(stack[0]->size);
    int half = msize >> 1;
    float* rPi = stack[0]->real;
    float* rPj = &stack[0]->real[msize - 1];
    float* iPi = stack[1]->real;
    float* iPj = &stack[1]->real[msize - 1];

    if (_sign > 0) {
        for (int i = 1, k = half; i < k; i++, rPi++, iPi++, rPj--, iPj--) {
            double a, b, c, d;
            double q, r, s, t;
            a = *rPi;
            b = *rPj;
            q = a + b;
            r = a - b;
            c = *iPi;
            d = *iPj;
            s = c + d;
            t = c - d;
            *rPi = (q + t) * .5;
            *rPj = (q - t) * .5;
            *iPi = (s - r) * .5;
            *iPj = (s + r) * .5;
        }
        fht(stack[0]->real, msize);
        fht(stack[1]->real, msize);
    } else {
        fht(stack[0]->real, msize);
        fht(stack[1]->real, msize);
        for (int i = 1, k = half; i < k; i++, rPi++, iPi++, rPj--, iPj--) {
            double a, b, c, d;
            double q, r, s, t;
            a = *rPi;
            b = *rPj;
            q = a + b;
            r = a - b;
            c = *iPi;
            d = *iPj;
            s = c + d;
            t = c - d;
            *iPi = (s + r) * 0.5;
            *iPj = (s - r) * 0.5;
            *rPi = (q - t) * 0.5;
            *rPj = (q + t) * 0.5;
        }
    }
}

void fht(REAL* fz, int size) {
    int k;
    REAL *fi, *fn, *gi;
    TRIG_VARS;

    for (int k1 = 1, k2 = 0; k1 < size; k1++) {
        REAL a;
        for (int k = size >> 1; (!((k2 ^= k) & k)); k >>= 1)
            ;
        if (k1 > k2) {
            a = fz[k1];
            fz[k1] = fz[k2];
            fz[k2] = a;
        }
    }
    for (k = 0; (1 << k) < size; k++)
        ;
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
        for (int i = 1; i < kx; i++) {
            REAL c2, s2;
            TRIG_NEXT(k, c1, s1);
            c2 = c1 * c1 - s1 * s1;
            s2 = 2 * (c1 * s1);
            fn = fz + size;
            fi = fz + i;
            gi = fz + k1 - i;
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

    int length = sq(stack[0]->size);
    for (float* fPtr = stack[0]->real; fPtr < stack[0]->real + length; fPtr++)
        if (*fPtr < minHeight)
            *fPtr = minHeight;
}

void NativeLandscape::clipMax(float maxHeight) {
    if (stack[0]->size < 1) {
        std::snprintf(mExecString, sizeof(mExecString), "CLIP_MAX: Nothing on the stack");
        mExecStatus = EXEC_FAILURE;
        return;
    }

    int length = sq(stack[0]->size);
    for (float* fPtr = stack[0]->real; fPtr < stack[0]->real + length; fPtr++)
        if (*fPtr > maxHeight)
            *fPtr = maxHeight;
}
