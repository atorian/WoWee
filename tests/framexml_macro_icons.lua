-- Native APIs and real action-button Lua; no replacement GetMacroInfo/GetActionTexture.
local question = 'Interface\\Icons\\INV_Misc_QuestionMark'
local fire = 'Interface\\Icons\\Spell_Fire_FlameBolt'
local frost = 'Interface\\Icons\\Spell_Frost_FrostBolt02'
local fireName, fireRank = GetSpellInfo(133)
local frostName = GetSpellInfo(116)
assert(fireName and frostName, 'learned spell names are unavailable')
local function translated(body)
    return (body:gsub('Fireball', fireName):gsub('Frostbolt', frostName):gsub('Rank 1', fireRank))
end
local function check(body, expected, icon)
    body = translated(body)
    local index = CreateMacro('Icon regression', icon or question, body, nil)
    assert(index and index > 0, 'macro creation failed')
    PickupMacro(index)
    PlaceAction(1)
    ClearCursor()
    local _, texture = GetMacroInfo(index)
    assert(texture == expected, body .. ': macro icon = ' .. tostring(texture))
    assert(GetActionTexture(1) == expected, body .. ': action icon differs')
    ActionButton_Update(ActionButton1)
    assert(ActionButton1Icon:GetTexture() == expected, 'button did not receive resolved icon')
    assert(GetMacroBody(index) == body, 'reading the icon changed the macro')
    DeleteMacro(index)
end

check('#showtooltip Fireball\n/cast Frostbolt\n/cast Fireball', fire)
check('#show Frostbolt\n/cast Fireball', frost)
check('#showtooltip\n/startattack\n/cast Fireball', fire)
check('/cast Frostbolt', frost)
check('  #showtooltip\tFireball\r\n/cast Frostbolt', fire)
check('#showtooltip [combat] Frostbolt; Fireball\n/cast Frostbolt', fire)
check('/cast [combat] Frostbolt\n/cast Fireball', fire)
check('/cast !Fireball', fire)
check('/cast Fireball(Rank 1)', fire)
check('#showtooltip Fireball\n/cast Fireball', frost, frost)
check('#showtooltip Unlearned Spell\n/cast Fireball', question)
check('/say hello', question)
check('#showtoolpit Frostbolt\n/cast Fireball', fire)

-- Editing an existing button must refresh it without picking it up again.
local index = CreateMacro('Edited icon', question, translated('#showtooltip Fireball'), nil)
PickupMacro(index) PlaceAction(1) ClearCursor()
EditMacro(index, nil, nil, translated('#showtooltip Frostbolt'))
assert(GetActionTexture(1) == frost, 'edit retained the old icon')
DeleteMacro(index)

if GetLocale() == 'ruRU' then
    assert(fireName == 'Огненный шар', 'Spell.dbc locale was not selected')
    assert(SPELLBOOK == 'Заклинания', 'original FrameXML localization was not loaded')
    local warlock = {
        {348, 'Жертвенный огонь', 'Spell_Fire_Immolation'},
        {6353, 'Ожог души', 'Spell_Fire_Fireball02'},
        {29722, 'Испепеление', 'Spell_Fire_Burnout'},
        {686, 'Стрела Тьмы', 'Spell_Shadow_ShadowBolt'},
        {603, 'Проклятие рока', 'Spell_Shadow_AuraOfDarkness'},
        {172, 'Порча', 'Spell_Shadow_AbominationExplosion'},
    }
    for _, spell in ipairs(warlock) do
        if IsSpellKnown(spell[1]) then
            check('#showtooltip ' .. spell[2] .. '\n/startattack\n/cast Демоническое могущество\n/cast ' .. spell[2],
                  'Interface\\Icons\\' .. spell[3])
        end
    end
end
