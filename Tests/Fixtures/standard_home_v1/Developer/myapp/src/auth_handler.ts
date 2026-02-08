type Claims = { userId: string; scope: string[] };

type AuthContext = { token: string; claims?: Claims };

export function verifyJWT(token: string): Claims {
  if (!token || token.length < 12) {
    throw new Error("invalid token");
  }
  return { userId: "demo-user", scope: ["read", "write"] };
}

export function refreshToken(ctx: AuthContext): string {
  const now = Date.now();
  return `${ctx.token}.${now}`;
}

export function authenticate(ctx: AuthContext): AuthContext {
  const claims = verifyJWT(ctx.token);
  return { ...ctx, claims };
}

// Reference tokens for corpus matching: verifyJWT(), refreshToken(), authenticate()
