// svedit: Scarlet / Violet save edits through PKHeX.Core (NuGet, 25.12.21: the last release for .NET 9). The game must
// be closed: it saves over the file otherwise. Back the save folder up first.
//
//   dotnet run -- flags <save>\main <key hex>...   turn boolean save flags on (SCBlock keys, PKHeX's
//                                                  SaveBlockAccessor9SV names; = (uint)FnvHash.HashFnv1a_64(name))
//   dotnet run -- items <save>\main <out>          write items.txt for the item mod: "id \t pocket \t name"
//   dotnet run -- find <save>\main <regex>         list PKHeX's block names matching regex, their keys and state
//
// Keys used so far: ride upgrades dash 0D707154, swim 76FF4AF0, high jump 54B8E8E1, glide 9DDF953F, climb 6ABB01F7,
// fly 3A9255B2, flight controls option E1F8DEBA; raid levels 3-6 EC95D8EF A9428DFE 9535F471 6E7F8220. Bag: KItem
// 21C9BD44 = 3000 x {u32 pocket, u32 count, u32 flags (1 new, 2 favourite, 4 obtained), u32 0}, by item id.
using System.Reflection;
using System.Text.RegularExpressions;
using PKHeX.Core;

if (args.Length < 2) { Console.Error.WriteLine("usage: flags <main> <key>... | items <main> <out> | find <main> <regex>"); return 1; }
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
