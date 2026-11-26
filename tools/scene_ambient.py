"""Select the resident ambient bank from exported SceneManager settings."""


def ambient_zone(scene):
    # ParticleEffects controller resources.assets-20837 has no zone-5 override;
    # its default resources.assets-4381 is the existing ambient-particles.bin.
    # Zones 9/10/25 use the recovered controller override banks.
    override = scene.get('overrideParticlesWith', 0)
    if isinstance(override, dict):
        override = override.get('m_PathID', 0)
    zone = scene.get('mapZone', 0)
    return zone if (zone in (5, 9, 10, 25) and not scene.get('noParticles', True)
                    and override == 0) else 0


def default_ambient_enabled(scene):
    return bool(ambient_zone(scene))
