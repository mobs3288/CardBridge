#pragma once

// A small reference for what a Bullet-like engine does with the exported rigid bodies and joints: planar rigid bodies (x, y, angle) with
// gravity, linear and angular damping as btRigidBody applies them, pin joints solved by sequential impulses and the 6-DOF joint's angular
// spring about its Z axis (torque = stiffness * angle, the Bullet spring) with the joint's rotation limits as a hard stop (the relative
// angular velocity into the limit is removed, as a limit constraint does; a stiff spring-like stop goes unstable when it is hit often). It reads the
// bodies and joints through the independent PMX reader, so what it settles is what a PMX viewer would be handed. Planar only: the chain
// must swing in the X-Y plane (the joint frames' Z axis along the world Z). Not Bullet: no contacts, no 3D, an approximation of its solver.

#include "PmxReader.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace springref {

struct V2 { double x = 0, y = 0; };
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, double s) { return {a.x * s, a.y * s}; }
inline double dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(V2 a, V2 b) { return a.x * b.y - a.y * b.x; }
inline V2 rot(double angle, V2 v) { const double c = std::cos(angle), s = std::sin(angle); return {c * v.x - s * v.y, s * v.x + c * v.y}; }
inline V2 perp(double w, V2 r) { return {-w * r.y, w * r.x}; }

struct Body {
    bool dynamic = false;
    double invMass = 0, invInertia = 0, linearDamping = 0, angularDamping = 0;
    V2 c, v;                 // centre, velocity
    double angle = 0, w = 0;
    V2 shift;                // kinematic bodies: offset from the rest position
    V2 rest;
};

struct Pin {
    int a = -1, b = -1;
    V2 ra, rb;               // anchors in each body's frame (at the start pose)
    double stiffness = 0, lower = 0, upper = 0;
    double angle0 = 0;       // relative angle at the start
};

class Chain {
public:
    // Builds the world from the file's rigid bodies and joints (in the model's units); gravity in units a second squared.
    Chain(const pmxread::Model& m, double gravity) : g_(gravity) {
        for (const auto& rb : m.bodies) {
            Body b;
            b.c = b.rest = {rb.pos.x, rb.pos.y};
            b.dynamic = rb.mode != 0;
            if (b.dynamic) {
                b.invMass = 1.0 / rb.mass;
                // btCapsuleShape / btSphereShape inertia: the bounding box of the shape about its centre (capsule), a solid sphere
                double inertia;
                if (rb.shape == 2) {
                    const double lx = 2.0 * rb.size.x, ly = rb.size.y + 2.0 * rb.size.x;
                    inertia = rb.mass / 12.0 * (ly * ly + lx * lx);
                } else {
                    inertia = 0.4 * rb.mass * rb.size.x * rb.size.x;
                }
                b.invInertia = 1.0 / inertia;
                b.linearDamping = rb.moveDamping;
                b.angularDamping = rb.rotDamping;
            }
            b.angle = 0;   // the bodies stand at their rest angle: the springs' equilibrium
            bodies_.push_back(b);
        }
        for (const auto& j : m.joints) {
            Pin p;
            p.a = j.a; p.b = j.b;
            const V2 at{j.pos.x, j.pos.y};
            p.ra = at - bodies_[static_cast<size_t>(j.a)].c;
            p.rb = at - bodies_[static_cast<size_t>(j.b)].c;
            p.stiffness = j.springRot.z;
            p.lower = j.rotLower.z;
            p.upper = j.rotUpper.z;
            pins_.push_back(p);
        }
    }

    // Moves every kinematic body by `shift` (a model-space offset from where it started).
    void moveAnchors(V2 shift) {
        for (auto& b : bodies_)
            if (!b.dynamic) {
                b.shift = shift;
                b.v = ((b.rest + shift) - b.c) * (1.0 / frame_);   // it glides there over the next frame
            }
    }

    // One 1/60 s frame (four substeps).
    void step() {
        const double dt = frame_ / 4.0;
        for (int s = 0; s < 4; ++s) substep(dt);
        for (auto& b : bodies_) if (!b.dynamic) { b.c = b.rest + b.shift; b.v = {}; }
    }

    const Body& body(size_t i) const { return bodies_[i]; }
    const Pin& pin(size_t i) const { return pins_[i]; }
    size_t pinCount() const { return pins_.size(); }
    // The relative angle of a joint's second body to its first.
    double relativeAngle(size_t pin) const { return bodies_[static_cast<size_t>(pins_[pin].b)].angle - bodies_[static_cast<size_t>(pins_[pin].a)].angle - pins_[pin].angle0; }
    // World position of a joint (the second body's anchor).
    V2 jointPoint(size_t pin) const { const Body& b = bodies_[static_cast<size_t>(pins_[pin].b)]; return b.c + rot(b.angle, pins_[pin].rb); }
    // Largest speed of any dynamic body: centre and (arm 1 unit) rotation.
    double speed() const {
        double s = 0;
        for (const auto& b : bodies_) if (b.dynamic) s = std::max(s, std::sqrt(dot(b.v, b.v)) + std::fabs(b.w));
        return s;
    }

private:
    void substep(double dt) {
        for (auto& b : bodies_) if (b.dynamic) b.v.y -= g_ * dt;
        for (const auto& p : pins_) {   // spring torque and the stop, equal and opposite
            const double th = bodies_[static_cast<size_t>(p.b)].angle - bodies_[static_cast<size_t>(p.a)].angle - p.angle0;
            const double torque = -p.stiffness * th;
            Body& A = bodies_[static_cast<size_t>(p.a)];
            Body& B = bodies_[static_cast<size_t>(p.b)];
            B.w += torque * B.invInertia * dt;
            A.w -= torque * A.invInertia * dt;
            const double rel = B.w - A.w;
            if ((th > p.upper && rel > 0) || (th < p.lower && rel < 0)) {   // the stop
                const double imp = -rel / (A.invInertia + B.invInertia);
                B.w += imp * B.invInertia;
                A.w -= imp * A.invInertia;
            }
        }
        for (auto& b : bodies_) if (b.dynamic) {   // btRigidBody::applyDamping
            b.v = b.v * std::pow(1.0 - b.linearDamping, dt);
            b.w *= std::pow(1.0 - b.angularDamping, dt);
        }
        for (int it = 0; it < 24; ++it)
            for (const auto& p : pins_) {
                Body& A = bodies_[static_cast<size_t>(p.a)];
                Body& B = bodies_[static_cast<size_t>(p.b)];
                const V2 ra = rot(A.angle, p.ra), rb = rot(B.angle, p.rb);
                const V2 err = (B.c + rb) - (A.c + ra);
                const V2 va = A.v + perp(A.w, ra), vb = B.v + perp(B.w, rb);
                const V2 rel = (vb - va) + err * (0.2 / dt);
                // effective mass matrix K (2x2)
                const double kxx = A.invMass + B.invMass + A.invInertia * ra.y * ra.y + B.invInertia * rb.y * rb.y;
                const double kyy = A.invMass + B.invMass + A.invInertia * ra.x * ra.x + B.invInertia * rb.x * rb.x;
                const double kxy = -A.invInertia * ra.x * ra.y - B.invInertia * rb.x * rb.y;
                const double det = kxx * kyy - kxy * kxy;
                if (std::fabs(det) < 1e-18) continue;
                const V2 imp{-(kyy * rel.x - kxy * rel.y) / det, -(-kxy * rel.x + kxx * rel.y) / det};
                A.v = A.v - imp * A.invMass;
                A.w -= A.invInertia * cross(ra, imp);
                B.v = B.v + imp * B.invMass;
                B.w += B.invInertia * cross(rb, imp);
            }
        for (auto& b : bodies_) {
            b.c = b.c + b.v * dt;
            if (b.dynamic) b.angle += b.w * dt;
        }
    }

    double g_;
    double frame_ = 1.0 / 60.0;
    std::vector<Body> bodies_;
    std::vector<Pin> pins_;
};

} // namespace springref
