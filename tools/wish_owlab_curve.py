"""Generate the immutable Owlab Ti HE distance model used by curve ID 3.

The 3.4 mm stroke and 120/750 G endpoints are nominal switch specifications.
The WISH-HE reference cylinder (2 mm radius, 2 mm length) is a modeling
assumption. The fitted gap is not a mechanical dimension measured on the C86.
"""
import math


def signal_knots():
    stroke, rest_flux, bottom_flux = 3.4, 120.0, 750.0
    radius, length = 2.0, 2.0

    def axial_field(gap):
        far = gap + length
        return far / math.hypot(far, radius) - gap / math.hypot(gap, radius)

    def residual(gap):
        return rest_flux * axial_field(gap) - bottom_flux * axial_field(gap + stroke)

    low, high = 0.0, 20.0
    if not residual(low) > 0 > residual(high):
        raise ValueError("The nominal field ratio must bracket a physical gap")
    for _ in range(60):
        middle = (low + high) / 2
        if residual(middle) > 0:
            low = middle
        else:
            high = middle
    gap = (low + high) / 2
    rest, bottom = axial_field(gap + stroke), axial_field(gap)
    return tuple(math.floor(32767 * (axial_field(gap + stroke * (1 - i/32)) - rest)
                            / (bottom - rest) + 0.5) for i in range(33))


if __name__ == "__main__":
    print(", ".join(map(str, signal_knots())))
