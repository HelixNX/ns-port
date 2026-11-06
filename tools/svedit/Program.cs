// svedit: Scarlet / Violet save edits through PKHeX.Core (NuGet, 25.12.21: the last release for .NET 9). The game must
// be closed: it saves over the file otherwise. Back the save folder up first.
//
//   dotnet run -- flags <save>\main <key hex>...   turn boolean save flags on (SCBlock keys, PKHeX's
//                                                  SaveBlockAccessor9SV names; = (uint)FnvHash.HashFnv1a_64(name))
//   dotnet run -- items <save>\main <out>          write items.txt for the item mod: "id \t pocket \t name"
//   dotnet run -- find <save>\main <regex>         list PKHeX's block names matching regex, their keys and state
//   dotnet run -- addmon <save>\main <species> <level> <nature> <ability 0-2> <nickname> <m1,m2,m3,m4> <hp/atk/def/spa/spd/spe EVs>
//                                                  a made-up mon (full IVs, the save's trainer), party or first free box slot
//
// Keys used so far: ride upgrades dash 0D707154, swim 76FF4AF0, high jump 54B8E8E1, glide 9DDF953F, climb 6ABB01F7,
// fly 3A9255B2, flight controls option E1F8DEBA; raid levels 3-6 EC95D8EF A9428DFE 9535F471 6E7F8220. Bag: KItem
// 21C9BD44 = 3000 x {u32 pocket, u32 count, u32 flags (1 new, 2 favourite, 4 obtained), u32 0}, by item id.
using System.Reflection;
using System.Text.RegularExpressions;
using PKHeX.Core;

if (args.Length < 2) { Console.Error.WriteLine("usage: flags <main> <key>... | items <main> <out> | find <main> <regex> | addmon <main> <species> <level> <nature> <ability> <nick> <moves> <evs>"); return 1; }
var path = args[1];
var sav = (SAV9SV)SaveUtil.GetSaveFile(path)!;
switch (args[0])
{
    case "flags":
    {
        var keys = args.Skip(2).Select(k => Convert.ToUInt32(k, 16)).ToArray();
        foreach (var key in keys)
        {
            var b = sav.Accessor.GetBlock(key);
            var was = b.Type;
            if (b.Type == SCTypeCode.Bool1) b.ChangeBooleanType(SCTypeCode.Bool2);
            Console.WriteLine($"{key:X8}: {was} -> {b.Type}");
        }
        File.WriteAllBytes(path, sav.Write().ToArray());
        var check = (SAV9SV)SaveUtil.GetSaveFile(path)!;
        foreach (var key in keys) Console.WriteLine($"reloaded {key:X8}: {check.Accessor.GetBlock(key).Type}");
        return 0;
    }
    case "items":
    {
        var pocket = new Dictionary<InventoryType, int> {
            [InventoryType.Medicine] = 0, [InventoryType.Balls] = 1, [InventoryType.BattleItems] = 2, [InventoryType.Berries] = 3,
            [InventoryType.Items] = 4, [InventoryType.TMHMs] = 5, [InventoryType.Treasure] = 6, [InventoryType.Ingredients] = 7,
            [InventoryType.KeyItems] = 8, [InventoryType.Candy] = 9 };
        var lines = new List<string>();
        foreach (var p in sav.Inventory)
            foreach (var id in p.GetAllItems())
                lines.Add($"{id}\t{pocket[p.Type]}\t{GameInfo.Strings.Item[id]}");
        File.WriteAllLines(args[2], lines);
        Console.WriteLine($"{lines.Count} items -> {args[2]}");
        return 0;
    }
    case "addmon":
    {
        // addmon <main> <species> <level> <nature> <ability index 0-2> <nickname> <move,move,move,move> <evs hp/atk/def/spa/spd/spe>
        // full IVs, the save's trainer as OT, Poke Ball; into the party, else the first free box slot
        var s = GameInfo.Strings;
        int Find(IReadOnlyList<string> list, string name, string what)
        {
            for (int i = 0; i < list.Count; i++)
                if (string.Equals(list[i], name, StringComparison.OrdinalIgnoreCase)) return i;
            throw new Exception($"no {what} named {name}");
        }
        var species = (ushort)Find(s.Species, args[2], "species");
        var pk = new PK9 { Species = species, Form = 0 };
        var pi = pk.PersonalInfo;
        pk.PID = Util.Rand32();
        pk.EncryptionConstant = Util.Rand32();
        pk.Version = GameVersion.VL;
        pk.Language = sav.Language;
        pk.OriginalTrainerName = sav.OT;
        pk.OriginalTrainerGender = sav.Gender;
        pk.ID32 = sav.ID32;
        pk.Ball = (byte)Ball.Poke;
        pk.MetLocation = 6;                 // South Province (Area One)
        pk.MetLevel = 1;
        pk.MetDate = DateOnly.FromDateTime(DateTime.Now);
        pk.CurrentLevel = byte.Parse(args[3]);
        var nature = Enum.Parse<Nature>(args[4], true);
        pk.Nature = nature;
        pk.StatNature = nature;
        pk.SetAbilityIndex(int.Parse(args[5]));
        pk.Gender = pk.GetSaneGender();
        pk.Nickname = args[6];
        pk.IsNicknamed = true;
        var moves = args[7].Split(',').Select(m => (ushort)Find(s.Move, m.Trim(), "move")).ToArray();
        pk.SetMoves(moves);
        pk.SetMaximumPPCurrent(moves);
        pk.IVs = [31, 31, 31, 31, 31, 31];
        var ev = args[8].Split('/').Select(int.Parse).ToArray();   // hp atk def spa spd spe
        pk.EV_HP = ev[0]; pk.EV_ATK = ev[1]; pk.EV_DEF = ev[2]; pk.EV_SPA = ev[3]; pk.EV_SPD = ev[4]; pk.EV_SPE = ev[5];
        pk.TeraTypeOriginal = (MoveType)pi.Type1;
        pk.TeraTypeOverride = (MoveType)TeraTypeUtil.OverrideNone;
        pk.HeightScalar = (byte)Util.Rand.Next(256);
        pk.WeightScalar = (byte)Util.Rand.Next(256);
        pk.Scale = (byte)Util.Rand.Next(256);
        pk.OriginalTrainerFriendship = pi.BaseFriendship;
        pk.HandlingTrainerName = "";
        pk.CurrentHandler = 0;
        pk.ResetPartyStats();
        pk.RefreshChecksum();
        if (sav.PartyCount < 6)
        {
            Console.WriteLine($"party slot {sav.PartyCount + 1}");
            sav.SetPartySlotAtIndex(pk, sav.PartyCount);
        }
        else
        {
            var slot = sav.NextOpenBoxSlot();
            if (slot < 0) throw new Exception("party and boxes full");
            sav.SetBoxSlotAtIndex(pk, slot);
            Console.WriteLine($"party full: box {slot / sav.BoxSlotCount + 1}, slot {slot % sav.BoxSlotCount + 1}");
        }
        Console.WriteLine($"{s.Species[species]} \"{pk.Nickname}\" Lv {pk.CurrentLevel} {pk.Nature} {s.Ability[pk.Ability]} " +
                          $"IVs {string.Join('/', pk.IVs)} EVs {pk.EV_HP}/{pk.EV_ATK}/{pk.EV_DEF}/{pk.EV_SPA}/{pk.EV_SPD}/{pk.EV_SPE} " +
                          $"moves {string.Join(", ", pk.Moves.Select(m => s.Move[m]))} stats {pk.Stat_HPMax}/{pk.Stat_ATK}/{pk.Stat_DEF}/{pk.Stat_SPA}/{pk.Stat_SPD}/{pk.Stat_SPE}");
        File.WriteAllBytes(path, sav.Write().ToArray());
        var check = (SAV9SV)SaveUtil.GetSaveFile(path)!;
        Console.WriteLine($"reloaded: party {check.PartyCount}: {string.Join(", ", check.PartyData.Select(p => p.Nickname))}");
        return 0;
    }
    case "find":
    {
        var re = new Regex(args[2], RegexOptions.IgnoreCase);
        foreach (var f in typeof(SaveBlockAccessor9SV).GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static))
        {
            if (!f.IsLiteral || !re.IsMatch(f.Name)) continue;
            var key = Convert.ToUInt32(f.GetRawConstantValue());
            Console.WriteLine($"{f.Name} {key:X8} {(sav.Accessor.TryGetBlock(key, out var b) ? b!.Type.ToString() : "-")}");
        }
        return 0;
    }
}
Console.Error.WriteLine("unknown command " + args[0]);
return 1;
