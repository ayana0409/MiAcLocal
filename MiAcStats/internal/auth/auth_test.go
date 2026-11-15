package auth

import (
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"testing"
	"time"
)

func TestHasRequiredPermission(t *testing.T) {
	tests := []struct {
		name     string
		claims   map[string]any
		expected bool
	}{
		{
			name:     "empty claims",
			claims:   map[string]any{},
			expected: false,
		},
		{
			name:     "nil permissions",
			claims:   map[string]any{"permissions": nil},
			expected: false,
		},
		{
			name:     "string single matching",
			claims:   map[string]any{"permissions": "xiaomi-nwt-airconditioner"},
			expected: true,
		},
		{
			name:     "string single matching with uppercase/whitespace",
			claims:   map[string]any{"permissions": "  Xiaomi-NWT-AirConditioner  "},
			expected: true,
		},
		{
			name:     "string single different",
			claims:   map[string]any{"permissions": "other-permission"},
			expected: false,
		},
		{
			name:     "slice string matching",
			claims:   map[string]any{"permissions": []string{"Order.Admin", "xiaomi-nwt-airconditioner", "User.Read"}},
			expected: true,
		},
		{
			name:     "slice any matching",
			claims:   map[string]any{"permissions": []any{"Order.Admin", "xiaomi-nwt-airconditioner"}},
			expected: true,
		},
		{
			name:     "slice any not matching",
			claims:   map[string]any{"permissions": []any{"Order.Admin", "AbpIdentity.Users"}},
			expected: false,
		},
		{
			name:     "singular permission key",
			claims:   map[string]any{"permission": "xiaomi-nwt-airconditioner"},
			expected: true,
		},
		{
			name:     "capitalized Permissions key",
			claims:   map[string]any{"Permissions": []string{"xiaomi-nwt-airconditioner"}},
			expected: true,
		},
		{
			name:     "json array string",
			claims:   map[string]any{"permissions": "[\"xiaomi-nwt-airconditioner\"]"},
			expected: true,
		},
		{
			name:     "comma separated string",
			claims:   map[string]any{"permissions": "Order.Admin, xiaomi-nwt-airconditioner, User.Read"},
			expected: true,
		},
		{
			name:     "in scope string",
			claims:   map[string]any{"scope": "openid xiaomi-nwt-airconditioner profile"},
			expected: true,
		},
		{
			name:     "in Microsoft claim URI",
			claims:   map[string]any{"http://schemas.xmlsoap.org/ws/2005/05/identity/claims/permission": "xiaomi-nwt-airconditioner"},
			expected: true,
		},
		{
			name:     "custom claim key fallback",
			claims:   map[string]any{"custom_claim": "xiaomi-nwt-airconditioner"},
			expected: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			res := HasRequiredPermission(tt.claims)
			if res != tt.expected {
				t.Errorf("expected %v, got %v", tt.expected, res)
			}
		})
	}
}

func TestSessionEncryptDecrypt(t *testing.T) {
	key := DeriveKey("my-test-super-secret-key-12345")

	origSID := "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"

	encrypted, err := EncryptSessionID(origSID, key)
	if err != nil {
		t.Fatalf("EncryptSessionID failed: %v", err)
	}

	decrypted, err := DecryptSessionID(encrypted, key)
	if err != nil {
		t.Fatalf("DecryptSessionID failed: %v", err)
	}

	if decrypted != origSID {
		t.Errorf("expected session ID %q, got %q", origSID, decrypted)
	}

	// Tampered ciphertext must fail
	tampered := encrypted[:len(encrypted)-4] + "AAAA"
	_, err = DecryptSessionID(tampered, key)
	if err == nil {
		t.Error("expected error decrypting tampered session, got nil")
	}

	// Wrong key must fail
	wrongKey := DeriveKey("wrong-key")
	_, err = DecryptSessionID(encrypted, wrongKey)
	if err == nil {
		t.Error("expected error decrypting with wrong key, got nil")
	}
}

func TestSessionStore(t *testing.T) {
	store := NewSessionStore()

	data := &SessionData{
		Username:    "thuan",
		AccessToken: "test-access-token",
		ExpiresAt:   time.Now().Add(1 * time.Hour),
	}

	sid, err := store.Create(data)
	if err != nil {
		t.Fatalf("Create session failed: %v", err)
	}
	if len(sid) != 64 {
		t.Errorf("expected 64-char hex session id, got length %d", len(sid))
	}

	retrieved, ok := store.Get(sid)
	if !ok || retrieved == nil {
		t.Fatalf("expected session to exist")
	}
	if retrieved.Username != "thuan" {
		t.Errorf("expected username 'thuan', got %q", retrieved.Username)
	}

	// Update session
	retrieved.AccessToken = "updated-access-token"
	store.Update(sid, retrieved)

	updated, ok := store.Get(sid)
	if !ok || updated.AccessToken != "updated-access-token" {
		t.Errorf("expected updated access token, got %v", updated)
	}

	// Delete session
	store.Delete(sid)
	if _, ok := store.Get(sid); ok {
		t.Errorf("expected session to be deleted")
	}

	// Expired session
	expData := &SessionData{
		Username:  "expired-user",
		ExpiresAt: time.Now().Add(-1 * time.Minute),
	}
	expSID, _ := store.Create(expData)
	if _, ok := store.Get(expSID); ok {
		t.Errorf("expected expired session to be invalid")
	}
}

func TestEncryptedCookieSize(t *testing.T) {
	key := DeriveKey("my-test-super-secret-key-12345")
	sid := "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"

	encrypted, err := EncryptSessionID(sid, key)
	if err != nil {
		t.Fatalf("EncryptSessionID failed: %v", err)
	}

	t.Logf("Encrypted cookie length: %d bytes (RFC 6265 limit is 4096 bytes)", len(encrypted))
	if len(encrypted) > 256 {
		t.Errorf("expected small cookie size, got %d bytes", len(encrypted))
	}
}

type mockKeyFetcher struct {
	key *rsa.PublicKey
}

func (m *mockKeyFetcher) GetPublicKey(ctx context.Context, kid string) (*rsa.PublicKey, error) {
	return m.key, nil
}

func TestVerifyRS256(t *testing.T) {
	// Generate test RSA key pair
	privKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate rsa key: %v", err)
	}

	fetcher := &mockKeyFetcher{key: &privKey.PublicKey}

	// Helper to craft signed JWT
	makeJWT := func(hdr map[string]any, payload map[string]any) string {
		hJSON, _ := json.Marshal(hdr)
		pJSON, _ := json.Marshal(payload)
		hB64 := base64.RawURLEncoding.EncodeToString(hJSON)
		pB64 := base64.RawURLEncoding.EncodeToString(pJSON)
		input := hB64 + "." + pB64

		hasher := sha256.New()
		hasher.Write([]byte(input))
		digest := hasher.Sum(nil)
		sig, _ := rsa.SignPKCS1v15(rand.Reader, privKey, crypto.SHA256, digest)
		sB64 := base64.RawURLEncoding.EncodeToString(sig)
		return input + "." + sB64
	}

	// 1. Valid Token
	validToken := makeJWT(
		map[string]any{"alg": "RS256", "kid": "key-1", "typ": "JWT"},
		map[string]any{
			"sub":         "user-123",
			"permissions": []string{"xiaomi-nwt-airconditioner"},
			"exp":         time.Now().Add(10 * time.Minute).Unix(),
		},
	)

	claims, err := VerifyRS256(context.Background(), validToken, fetcher)
	if err != nil {
		t.Fatalf("expected valid token, got error: %v", err)
	}
	if !HasRequiredPermission(claims) {
		t.Errorf("expected permission to be present")
	}

	// 2. Expired Token
	expiredToken := makeJWT(
		map[string]any{"alg": "RS256", "kid": "key-1", "typ": "JWT"},
		map[string]any{
			"sub":         "user-123",
			"permissions": []string{"xiaomi-nwt-airconditioner"},
			"exp":         time.Now().Add(-10 * time.Minute).Unix(),
		},
	)
	_, err = VerifyRS256(context.Background(), expiredToken, fetcher)
	if err == nil || err.Error() != "token has expired" {
		t.Errorf("expected expired error, got %v", err)
	}

	// 3. Invalid Signature (modified payload)
	parts := base64.RawURLEncoding.EncodeToString([]byte(`{"sub":"tampered"}`))
	tamperedToken := validToken[:len(validToken)/2] + parts + validToken[len(validToken)/2:]
	_, err = VerifyRS256(context.Background(), tamperedToken, fetcher)
	if err == nil {
		t.Error("expected error for tampered token, got nil")
	}
}

