# PS1Emulator

Порт емулятора PlayStation (**PCSX-ReARMed**) під Symbian S60v3 / Belle для пристроїв Nokia на ARM11 (ARMv6). Мета — іграбельна швидкодія через динамічну рекомпіляцію MIPS R3000A → ARM з адаптацією під platform security Symbian (`RChunk` замість `mmap`, `User::IMB_Range()` замість `__clear_cache`).

- Правила й архітектура: [CLAUDE.md](./CLAUDE.md)
- Повний план: [pcsx-rearmed-symbian-port-plan.md](./pcsx-rearmed-symbian-port-plan.md)
- Поточний стан: `plans/progress.md`

Ліцензія похідного коду в `pcsx_rearmed/` — GPL-2.0.
