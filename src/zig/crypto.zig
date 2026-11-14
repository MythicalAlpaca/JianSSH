const std = @import("std");
const Aead = std.crypto.aead.chacha_poly.XChaCha20Poly1305;
// Argon2's temporary memory is password-derived: zero before returning pages.
const WipingAllocator = struct {
    fn alloc(_: *anyopaque, len: usize, alignment: std.mem.Alignment, ra: usize) ?[*]u8 { return std.heap.page_allocator.rawAlloc(len,alignment,ra); }
    fn free(_: *anyopaque, memory: []u8, alignment: std.mem.Alignment, ra: usize) void { std.crypto.secureZero(u8,memory); std.heap.page_allocator.rawFree(memory,alignment,ra); }
    const vtable: std.mem.Allocator.VTable = .{ .alloc=alloc, .resize=std.mem.Allocator.noResize, .remap=std.mem.Allocator.noRemap, .free=free };
    fn allocator() std.mem.Allocator { return .{ .ptr=undefined, .vtable=&vtable }; }
};
export fn js_crypto_wipe(p: [*]u8, n: usize) void { std.crypto.secureZero(u8, p[0..n]); }
export fn js_crypto_random(p: [*]u8, n: usize) c_int {
    var threaded = std.Io.Threaded.init(std.heap.page_allocator, .{});
    defer threaded.deinit();
    threaded.io().randomSecure(p[0..n]) catch { js_crypto_wipe(p,n); return 1; };
    return 0;
}
export fn js_crypto_kdf(out: *[32]u8, pw: [*]const u8, n: usize, salt: *const [16]u8, m: u32, t: u32, lanes: u32) c_int {
    if (m < 8192 or m > 262144 or t < 1 or t > 10 or lanes != 1 or @as(u64,m) * t > 786432 or n == 0 or n > 4096) { js_crypto_wipe(out,32); return 1; }
    var threaded = std.Io.Threaded.init(std.heap.page_allocator, .{});
    defer threaded.deinit();
    std.crypto.pwhash.argon2.kdf(WipingAllocator.allocator(), out, pw[0..n], salt, .{ .m=m, .t=t, .p=1 }, .argon2id, threaded.io()) catch { js_crypto_wipe(out,32); return 1; };
    return 0;
}
export fn js_crypto_encrypt(c: [*]u8, tag: *[16]u8, p: [*]const u8, n: usize, ad: [*]const u8, an: usize, nonce: *const [24]u8, key: *const [32]u8) c_int {
    Aead.encrypt(c[0..n], tag, p[0..n], ad[0..an], nonce.*, key.*); return 0;
}
export fn js_crypto_decrypt(p: [*]u8, c: [*]const u8, n: usize, tag: *const [16]u8, ad: [*]const u8, an: usize, nonce: *const [24]u8, key: *const [32]u8) c_int {
    Aead.decrypt(p[0..n], c[0..n], tag.*, ad[0..an], nonce.*, key.*) catch { js_crypto_wipe(p,n); return 1; }; return 0;
}
test "stdlib XChaCha20Poly1305 published vector and tamper" {
    const key: [32]u8 = @splat(69); const nonce: [24]u8 = @splat(42);
    const msg = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    const ad = "Additional data";
    var c: [msg.len]u8 = undefined; var tag: [16]u8 = undefined; var out: [msg.len]u8 = undefined;
    _ = js_crypto_encrypt(&c,&tag,msg,msg.len,ad,ad.len,&nonce,&key);
    const hex = "994d2dd32333f48e53650c02c7a2abb8e018b0836d7175aec779f52e961780768f815c58f1aa52d211498db89b9216763f569c9433a6bbfcefb4d4a49387a4c5207fbb3b5a92b5941294df30588c6740d39dc16fa1f0e634f7246cf7cdcb978e44347d89381b7a74eb7084f754b90bde9aaf5a94b8f2a85efd0b50692ae2d425e234";
    var expected: [msg.len+16]u8 = undefined; _ = try std.fmt.hexToBytes(&expected,hex);
    try std.testing.expectEqualSlices(u8,expected[0..msg.len],&c); try std.testing.expectEqualSlices(u8,expected[msg.len..],&tag);
    try std.testing.expectEqual(@as(c_int,0),js_crypto_decrypt(&out,&c,c.len,&tag,ad,ad.len,&nonce,&key));
    try std.testing.expectEqualSlices(u8,msg,&out);
    try std.testing.expectEqual(@as(c_int,1),js_crypto_decrypt(&out,&c,c.len,&tag,"Bad",3,&nonce,&key));
    c[0] ^= 1; try std.testing.expectEqual(@as(c_int,1),js_crypto_decrypt(&out,&c,c.len,&tag,ad,ad.len,&nonce,&key));
}
test "RFC 9106 section 5.3 Argon2id official vector" {
    const pw: [32]u8 = @splat(1); const salt: [16]u8 = @splat(2); const secret: [8]u8 = @splat(3); const ad: [12]u8 = @splat(4);
    var dk: [32]u8 = undefined;
    try std.crypto.pwhash.argon2.kdf(WipingAllocator.allocator(), &dk, &pw, &salt, .{ .t=3, .m=32, .p=4, .secret=&secret, .ad=&ad }, .argon2id, std.testing.io);
    var want: [32]u8 = undefined; _ = try std.fmt.hexToBytes(&want,"0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
    try std.testing.expectEqualSlices(u8,&want,&dk);
}
test "KDF deterministic roundtrip and parameter caps" {
    const salt: [16]u8 = @splat(7); var a: [32]u8 = undefined; var b: [32]u8 = undefined;
    try std.testing.expectEqual(@as(c_int,0),js_crypto_kdf(&a,"test",4,&salt,8192,1,1));
    try std.testing.expectEqual(@as(c_int,0),js_crypto_kdf(&b,"test",4,&salt,8192,1,1));
    try std.testing.expectEqualSlices(u8,&a,&b);
    try std.testing.expectEqual(@as(c_int,1),js_crypto_kdf(&b,"test",4,&salt,262145,1,1));
    try std.testing.expectEqual(@as(c_int,1),js_crypto_kdf(&b,"test",4,&salt,8192,11,1));
    try std.testing.expectEqual(@as(c_int,1),js_crypto_kdf(&b,"test",4,&salt,8192,1,2));
}
