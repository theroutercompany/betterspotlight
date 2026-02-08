import { authenticate } from "../src/auth_handler";

describe("auth handler", () => {
  it("adds claims for valid token", () => {
    const ctx = authenticate({ token: "abcdefghijklmnop" });
    expect(ctx.claims?.userId).toBe("demo-user");
  });

  it("throws for invalid token", () => {
    expect(() => authenticate({ token: "tiny" })).toThrow();
  });
});
# note line 1
# note line 2
# note line 3
# note line 4
# note line 5
# note line 6
# note line 7
# note line 8
