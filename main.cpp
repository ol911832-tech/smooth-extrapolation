// Smooth Extrapolation - extrapolacion visual de cuadros para Geometry Dash (Geode)
//
// Solo mueve la posicion VISUAL (nodo de cocos) del jugador y de la capa de objetos
// despues de que el juego termina su paso de fisica, y la restaura antes del siguiente.
// No modifica m_position, velocidades, hitboxes ni entradas.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace {

struct Vec {
    float x = 0.f, y = 0.f;
    Vec() = default;
    Vec(float x, float y) : x(x), y(y) {}
    Vec(CCPoint const& p) : x(p.x), y(p.y) {}
    Vec operator+(Vec o) const { return { x + o.x, y + o.y }; }
    Vec operator-(Vec o) const { return { x - o.x, y - o.y }; }
    Vec operator*(float f) const { return { x * f, y * f }; }
    float len() const { return std::sqrt(x * x + y * y); }
    CCPoint pt() const { return CCPoint(x, y); }
};

struct Track {
    Vec real;        // posicion real (la del juego) tras el ultimo update
    Vec vel;         // desplazamiento medido por tick
    Vec corr;        // correccion suavizada tras un fallo de prediccion
    Vec lastVisual;  // ultima posicion dibujada
    float realRot = 0.f;
    float rotVel = 0.f;
    bool applied = false;
    bool valid = false;
    bool hasVisual = false;
};

struct State {
    Track p1, p2, cam;
    double accum = 0.0; // tiempo no simulado desde el ultimo tick (puede ser negativo)
    int idle = 0;       // cuadros seguidos sin ticks
    bool enabled = true, camera = true, rotation = true, smooth = true, wave = true;

    void reset() {
        p1 = {}; p2 = {}; cam = {};
        accum = 0.0;
        idle = 0;
    }
    void loadSettings() {
        auto m = Mod::get();
        enabled  = m->getSettingValue<bool>("enabled");
        camera   = m->getSettingValue<bool>("camera");
        rotation = m->getSettingValue<bool>("rotation");
        smooth   = m->getSettingValue<bool>("smooth-correction");
        wave     = m->getSettingValue<bool>("wave-trail");
    }
} s;

constexpr float kTeleport = 40.f;  // unidades por tick: por encima es teletransporte/respawn
constexpr float kMaxCorr = 3.f;    // limite de la correccion suavizada (unidades)
constexpr float kCorrDecay = 45.f; // velocidad a la que desaparece la correccion (1/s)
constexpr float kMaxRotVel = 30.f; // grados por tick

void restorePlayer(PlayerObject* p, Track& t) {
    if (!p || !t.applied) return;
    p->CCSprite::setPosition(t.real.pt());
    if (s.rotation) p->CCSprite::setRotation(t.realRot);
    t.applied = false;
}

void restoreAll(GJBaseGameLayer* gl) {
    restorePlayer(gl->m_player1, s.p1);
    restorePlayer(gl->m_player2, s.p2);
    if (s.cam.applied && gl->m_objectLayer) {
        gl->m_objectLayer->setPosition(s.cam.real.pt());
        s.cam.applied = false;
    }
}

// Actualiza la velocidad medida. Devuelve true si hubo un cambio brusco.
bool measure(Track& t, Vec before, Vec after, int n) {
    t.real = after;
    if (n <= 0) return false;
    Vec v = (after - before) * (1.f / n);
    bool abrupt = false;
    if (v.len() > kTeleport) {
        v = {};
        t.corr = {};
        t.hasVisual = false;
    } else if (t.valid) {
        float lim = 0.5f * std::max(v.len(), t.vel.len()) + 0.25f;
        abrupt = (v - t.vel).len() > lim;
    }
    t.vel = v;
    t.valid = true;
    return abrupt;
}

void applyPlayer(PlayerObject* p, Track& t, Vec before, float rotBefore, int n, float alpha, float frameTicks, float dt) {
    if (!p) return;
    bool abrupt = measure(t, before, Vec(p->getPosition()), n);
    t.realRot = p->getRotation();
    if (n > 0) {
        float rv = (t.realRot - rotBefore) / n;
        t.rotVel = std::fabs(rv) > kMaxRotVel ? 0.f : rv;
    }
    if (p->m_isDead || !t.valid) {
        t.corr = {};
        t.hasVisual = false;
        return;
    }

    Vec off = t.vel * alpha;

    // No predecir caida hacia el suelo si ya esta apoyado (evita hundirse en bloques)
    bool grounded = p->m_isOnGround;
    bool falling = p->m_isUpsideDown ? t.vel.y > 0.f : t.vel.y < 0.f;
    bool clampY = grounded && falling;
    if (clampY) off.y = 0.f;

    Vec target = t.real + off;

    if (s.smooth) {
        if (abrupt && t.hasVisual) {
            // Diferencia entre donde "deberia" seguir el dibujo y la nueva prediccion
            Vec c = (t.lastVisual + t.vel * frameTicks) - target;
            float l = c.len();
            if (l > kMaxCorr) c = c * (kMaxCorr / l);
            t.corr = c;
        }
        t.corr = t.corr * std::exp(-dt * kCorrDecay);
        if (clampY) t.corr.y = 0.f;
        target = target + t.corr;
    }

    p->CCSprite::setPosition(target.pt());
    if (s.rotation) p->CCSprite::setRotation(t.realRot + t.rotVel * alpha);
    t.lastVisual = target;
    t.hasVisual = true;
    t.applied = true;

    // Que la punta del trail de wave siga al icono dibujado
    if (s.wave && p->m_isDart && p->m_waveTrail) {
        p->m_waveTrail->m_currentPoint = target.pt();
        p->m_waveTrail->updateStroke(0.f);
    }
}

} // namespace

class $modify(SEPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        s.reset();
        s.loadSettings();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        restoreAll(this);
        s.reset();
        PlayLayer::resetLevel();
    }
};

class $modify(SEBaseLayer, GJBaseGameLayer) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (!s.enabled || !pl || static_cast<GJBaseGameLayer*>(pl) != this || !m_player1 || !m_objectLayer) {
            GJBaseGameLayer::update(dt);
            return;
        }

        // 1. Devolver todo a su posicion real antes de que el juego calcule nada
        restoreAll(this);

        bool dual = m_gameState.m_isDualMode && m_player2;
        Vec b1 = m_player1->getPosition();
        float r1 = m_player1->getRotation();
        Vec b2;
        float r2 = 0.f;
        if (dual) { b2 = m_player2->getPosition(); r2 = m_player2->getRotation(); }
        Vec bc = m_objectLayer->getPosition();
        int tick0 = m_gameState.m_currentProgress;

        // 2. Fisica original, intacta
        GJBaseGameLayer::update(dt);

        // 3. Cuanto tiempo quedo sin simular
        int n = m_gameState.m_currentProgress - tick0;
        if (n < 0) n = 0;
        Vec a1 = m_player1->getPosition();
        if (n == 0 && (a1 - b1).len() > 0.0001f) n = 1; // respaldo si el contador no avanzo

        float tick = std::min(1.f, static_cast<float>(m_gameState.m_timeWarp)) / 240.f;
        if (tick <= 0.f) tick = 1.f / 240.f;

        s.accum += dt - n * tick;
        s.accum = std::clamp(s.accum, -static_cast<double>(tick), static_cast<double>(tick));
        float alpha = static_cast<float>(s.accum / tick);
        float frameTicks = dt / tick;

        if (n == 0) {
            if (++s.idle > 8) { // juego detenido: no seguir prediciendo
                s.p1.vel = {}; s.p2.vel = {}; s.cam.vel = {};
                s.p1.rotVel = 0.f; s.p2.rotVel = 0.f;
            }
        } else {
            s.idle = 0;
        }

        // 4. Aplicar el desplazamiento visual
        applyPlayer(m_player1, s.p1, b1, r1, n, alpha, frameTicks, dt);
        if (dual) applyPlayer(m_player2, s.p2, b2, r2, n, alpha, frameTicks, dt);
        else s.p2 = {};

        if (s.camera) {
            measure(s.cam, bc, Vec(m_objectLayer->getPosition()), n);
            if (s.cam.valid) {
                m_objectLayer->setPosition((s.cam.real + s.cam.vel * alpha).pt());
                s.cam.applied = true;
            }
        }
    }
};
