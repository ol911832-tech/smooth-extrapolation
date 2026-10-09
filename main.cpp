// Smooth Extrapolation v1.0.1 - extrapolacion visual de cuadros para Geometry Dash (Geode)
//
// Solo mueve la posicion VISUAL (nodo de cocos) del jugador y de la capa de objetos
// despues de que el juego termina su paso de fisica, y la restaura antes del siguiente.
// No modifica m_position, velocidades, hitboxes ni entradas.
//
// v1.0.1: si una animacion del juego (giro del cubo, animacion de fin de nivel) mueve
// o gira al jugador, el mod ya no la pisa: la respeta y no extrapola ese cuadro.

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
    Vec lastVisual;  // ultima posicion dibujada por el mod
    float realRot = 0.f;
    float rotVel = 0.f;
    float setRot = 0.f; // ultima rotacion puesta por el mod
    bool applied = false;
    bool rotApplied = false;
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
constexpr float kEps = 0.01f;

// Devuelve el nodo a su valor real, salvo que el juego lo haya cambiado por su cuenta
// (una accion/animacion). En ese caso no se toca y devuelve true.
bool restorePlayer(PlayerObject* p, Track& t) {
    if (!p) return false;
    bool external = false;
    if (t.applied) {
        t.applied = false;
        if ((Vec(p->getPosition()) - t.lastVisual).len() > kEps) external = true;
        else p->CCSprite::setPosition(t.real.pt());
    }
    if (t.rotApplied) {
        t.rotApplied = false;
        if (std::fabs(p->getRotation() - t.setRot) < kEps) p->CCSprite::setRotation(t.realRot);
    }
    return external;
}

void restoreCam(GJBaseGameLayer* gl) {
    if (!s.cam.applied || !gl->m_objectLayer) return;
    s.cam.applied = false;
    if ((Vec(gl->m_objectLayer->getPosition()) - s.cam.lastVisual).len() <= kEps)
        gl->m_objectLayer->setPosition(s.cam.real.pt());
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

void applyPlayer(PlayerObject* p, Track& t, Vec before, float rotBefore, int n, float alpha, float frameTicks, float dt, bool external) {
    if (!p) return;
    Vec oldVel = t.vel;
    bool abrupt = measure(t, before, Vec(p->getPosition()), n);
    t.realRot = p->getRotation();
    if (n > 0) {
        float rv = (t.realRot - rotBefore) / n;
        t.rotVel = std::fabs(rv) > kMaxRotVel ? 0.f : rv;
    }
    // Muerto, o una animacion del juego esta moviendo al jugador: no tocar nada
    if (external || p->m_isDead || !t.valid) {
        if (external) { t.vel = {}; t.rotVel = 0.f; }
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
            // Diferencia entre donde iba a seguir el dibujo y la nueva prediccion
            Vec c = (t.lastVisual + oldVel * frameTicks) - target;
            float l = c.len();
            if (l > kMaxCorr) c = c * (kMaxCorr / l);
            t.corr = c;
        }
        t.corr = t.corr * std::exp(-dt * kCorrDecay);
        if (clampY) t.corr.y = 0.f;
        target = target + t.corr;
    }

    p->CCSprite::setPosition(target.pt());
    t.lastVisual = target;
    t.hasVisual = true;
    t.applied = true;

    // Solo se extrapola el giro que calcula la fisica (nave, wave...). El giro del cubo
    // lo hace una animacion del juego que ya es suave por cuadro, y no se toca.
    if (s.rotation && t.rotVel != 0.f) {
        t.setRot = t.realRot + t.rotVel * alpha;
        p->CCSprite::setRotation(t.setRot);
        t.rotApplied = true;
    }

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
        restorePlayer(m_player1, s.p1);
        restorePlayer(m_player2, s.p2);
        restoreCam(this);
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
        bool ext1 = restorePlayer(m_player1, s.p1);
        bool ext2 = restorePlayer(m_player2, s.p2);
        restoreCam(this);

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
        applyPlayer(m_player1, s.p1, b1, r1, n, alpha, frameTicks, dt, ext1);
        if (dual) applyPlayer(m_player2, s.p2, b2, r2, n, alpha, frameTicks, dt, ext2);
        else s.p2 = {};

        if (s.camera) {
            measure(s.cam, bc, Vec(m_objectLayer->getPosition()), n);
            if (s.cam.valid) {
                s.cam.lastVisual = s.cam.real + s.cam.vel * alpha;
                m_objectLayer->setPosition(s.cam.lastVisual.pt());
                s.cam.applied = true;
            }
        }
    }
};
