function Register()
    return "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 F0 FD FF FF 48 81 EC 10 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 00 02 00 00 4C 8B 31 48 8B F9 4C 8B 61 08 45 33 C9 8B 71 18 45 33 C0 4C 8B 79 28 ?? ?? 44 8B 69 70"
end

function OnMatchFound(MatchAddress)
    return MatchAddress
end
