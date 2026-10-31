#include <opent1/math/matrix.hpp>
#include <algorithm>

namespace opent1 {
EulerF& EulerF::set(const RMat3F& mat) {
    if (!(mat.flags & RMat3F::Matrix_HasRotation)) {
        return set();
    }
    const auto sinx = std::clamp(mat.m[1][2], -1.0f, 1.0f);
    x = m_asin(sinx);
    if (isEqual(sinx, 1.0f) || isEqual(sinx, -1.0f)) {
        y = 0;
        z = m_atan(mat.m[0][0], mat.m[0][1]);
    } else {
        y = m_atan(mat.m[2][2], -mat.m[0][2]);
        z = m_atan(mat.m[1][1], -mat.m[1][0]);
    }
    return *this;
}

RMat3F& EulerF::makeMatrix(RMat3F& mat) const {
    return mat.set(*this);
}
} // namespace opent1
