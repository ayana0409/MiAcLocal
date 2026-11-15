package auth

import (
	"context"
	"crypto"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"strings"
	"sync"
	"time"
)

// JWKS represents the response payload from /.well-known/jwks.
type JWKS struct {
	Keys []JWK `json:"keys"`
}

// JWK represents a single JSON Web Key.
type JWK struct {
	Kty string `json:"kty"`
	Use string `json:"use"`
	Alg string `json:"alg"`
	Kid string `json:"kid"`
	N   string `json:"n"`
	E   string `json:"e"`
}

// KeyFetcher provides public keys by Key ID.
type KeyFetcher interface {
	GetPublicKey(ctx context.Context, kid string) (*rsa.PublicKey, error)
}

// JWKSCache fetches and caches JWKS public keys from an OIDC provider.
type JWKSCache struct {
	jwksURL    string
	httpClient *http.Client
	mu         sync.RWMutex
	keys       map[string]*rsa.PublicKey
	fetchedAt  time.Time
	cacheTTL   time.Duration
}

// NewJWKSCache constructs a new JWKSCache for the given jwksURL.
func NewJWKSCache(jwksURL string, client *http.Client) *JWKSCache {
	if client == nil {
		client = &http.Client{Timeout: 10 * time.Second}
	}
	return &JWKSCache{
		jwksURL:    jwksURL,
		httpClient: client,
		keys:       make(map[string]*rsa.PublicKey),
		cacheTTL:   24 * time.Hour,
	}
}

// SetKey directly sets a key (useful for unit testing).
func (c *JWKSCache) SetKey(kid string, pub *rsa.PublicKey) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.keys[kid] = pub
	c.fetchedAt = time.Now()
}

// GetPublicKey retrieves the cached RSA public key or refreshes from the endpoint.
func (c *JWKSCache) GetPublicKey(ctx context.Context, kid string) (*rsa.PublicKey, error) {
	c.mu.RLock()
	key, found := c.keys[kid]
	isStale := time.Since(c.fetchedAt) > c.cacheTTL
	c.mu.RUnlock()

	if found && !isStale {
		return key, nil
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	// Double-check after acquiring write lock
	if key, found = c.keys[kid]; found && time.Since(c.fetchedAt) <= c.cacheTTL {
		return key, nil
	}

	if err := c.fetchLocked(ctx); err != nil {
		if found {
			// Fallback to stale key if network fetch fails
			return key, nil
		}
		return nil, fmt.Errorf("fetch JWKS failed: %w", err)
	}

	key, found = c.keys[kid]
	if !found {
		return nil, fmt.Errorf("key id %q not found in JWKS", kid)
	}
	return key, nil
}

func (c *JWKSCache) fetchLocked(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.jwksURL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "application/json")

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("unexpected HTTP %d from %s", resp.StatusCode, c.jwksURL)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return err
	}

	var jwks JWKS
	if err := json.Unmarshal(body, &jwks); err != nil {
		return fmt.Errorf("invalid JWKS JSON: %w", err)
	}

	newKeys := make(map[string]*rsa.PublicKey)
	for _, k := range jwks.Keys {
		if (k.Kty == "" || k.Kty == "RSA") && k.N != "" && k.E != "" {
			pubKey, err := parseRSAPublicKey(k.N, k.E)
			if err == nil {
				newKeys[k.Kid] = pubKey
			}
		}
	}

	c.keys = newKeys
	c.fetchedAt = time.Now()
	return nil
}

func parseRSAPublicKey(nStr, eStr string) (*rsa.PublicKey, error) {
	nBytes, err := base64.RawURLEncoding.DecodeString(nStr)
	if err != nil {
		// Try standard URL encoding with padding
		nBytes, err = base64.URLEncoding.DecodeString(nStr)
		if err != nil {
			return nil, fmt.Errorf("decode modulus: %w", err)
		}
	}

	eBytes, err := base64.RawURLEncoding.DecodeString(eStr)
	if err != nil {
		eBytes, err = base64.URLEncoding.DecodeString(eStr)
		if err != nil {
			return nil, fmt.Errorf("decode exponent: %w", err)
		}
	}

	n := new(big.Int).SetBytes(nBytes)
	var e int
	for _, b := range eBytes {
		e = (e << 8) | int(b)
	}

	return &rsa.PublicKey{N: n, E: e}, nil
}

// JWTHeader holds common JWT header fields.
type JWTHeader struct {
	Alg string `json:"alg"`
	Kid string `json:"kid"`
	Typ string `json:"typ"`
}

// ParseUnverifiedClaims decodes the payload of a JWT without verifying its signature.
func ParseUnverifiedClaims(tokenStr string) (map[string]any, error) {
	parts := strings.Split(tokenStr, ".")
	if len(parts) != 3 {
		return nil, errors.New("invalid token format: must have 3 parts")
	}

	payloadBytes, err := decodeBase64URL(parts[1])
	if err != nil {
		return nil, fmt.Errorf("decode payload: %w", err)
	}

	var claims map[string]any
	if err := json.Unmarshal(payloadBytes, &claims); err != nil {
		return nil, fmt.Errorf("unmarshal claims: %w", err)
	}

	return claims, nil
}

// VerifyRS256 verifies an RS256-signed JWT using the provided KeyFetcher.
// Returns the decoded claims map if valid.
func VerifyRS256(ctx context.Context, tokenStr string, fetcher KeyFetcher) (map[string]any, error) {
	parts := strings.Split(tokenStr, ".")
	if len(parts) != 3 {
		return nil, errors.New("invalid token format: must have 3 parts")
	}

	headerBytes, err := decodeBase64URL(parts[0])
	if err != nil {
		return nil, fmt.Errorf("decode header: %w", err)
	}

	var hdr JWTHeader
	if err := json.Unmarshal(headerBytes, &hdr); err != nil {
		return nil, fmt.Errorf("unmarshal header: %w", err)
	}

	if hdr.Alg != "RS256" {
		return nil, fmt.Errorf("unsupported algorithm: %s (expected RS256)", hdr.Alg)
	}

	pubKey, err := fetcher.GetPublicKey(ctx, hdr.Kid)
	if err != nil {
		return nil, fmt.Errorf("get public key for kid %q: %w", hdr.Kid, err)
	}

	sigBytes, err := decodeBase64URL(parts[2])
	if err != nil {
		return nil, fmt.Errorf("decode signature: %w", err)
	}

	signingInput := []byte(parts[0] + "." + parts[1])
	hasher := sha256.New()
	hasher.Write(signingInput)
	digest := hasher.Sum(nil)

	if err := rsa.VerifyPKCS1v15(pubKey, crypto.SHA256, digest, sigBytes); err != nil {
		return nil, errors.New("invalid signature")
	}

	payloadBytes, err := decodeBase64URL(parts[1])
	if err != nil {
		return nil, fmt.Errorf("decode payload: %w", err)
	}

	var claims map[string]any
	if err := json.Unmarshal(payloadBytes, &claims); err != nil {
		return nil, fmt.Errorf("unmarshal claims: %w", err)
	}

	// Verify expiration if present
	if expRaw, ok := claims["exp"]; ok {
		var expSec int64
		switch v := expRaw.(type) {
		case float64:
			expSec = int64(v)
		case json.Number:
			expSec, _ = v.Int64()
		}
		if expSec > 0 && time.Now().Unix() > expSec {
			return nil, errors.New("token has expired")
		}
	}

	return claims, nil
}

func decodeBase64URL(s string) ([]byte, error) {
	b, err := base64.RawURLEncoding.DecodeString(s)
	if err == nil {
		return b, nil
	}
	return base64.URLEncoding.DecodeString(s)
}

