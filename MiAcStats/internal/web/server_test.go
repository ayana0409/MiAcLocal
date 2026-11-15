package web

import (
	"bytes"
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"

	"miacstats/internal/auth"
	"miacstats/internal/config"
	"miacstats/internal/model"
)

type memoryStore struct {
	buckets  []model.Bucket
	settings model.BillingSettings
}

func (m *memoryStore) EnsureSchema(context.Context) error { return nil }

func (m *memoryStore) QueryBuckets(_ context.Context, deviceID string, fromUTC, toUTC int64) ([]model.Bucket, error) {
	var out []model.Bucket
	for _, b := range m.buckets {
		if b.DeviceID == deviceID && b.TsUTC >= fromUTC && b.TsUTC < toUTC {
			out = append(out, b)
		}
	}
	return out, nil
}

func (m *memoryStore) UpsertBuckets(_ context.Context, buckets []model.Bucket) error {
	m.buckets = append(m.buckets, buckets...)
	return nil
}

func (m *memoryStore) LoadBillingSettings(_ context.Context, fallback model.BillingSettings) (model.BillingSettings, error) {
	if m.settings.CoolWatts == 0 {
		return fallback, nil
	}
	return m.settings, nil
}

func (m *memoryStore) SaveBillingSettings(_ context.Context, cfg model.BillingSettings) error {
	m.settings = cfg
	return nil
}

func TestDashboardRoutesAndQueries(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	cfg := config.Config{
		Port:            "8080",
		DefaultDeviceID: "miac-main",
		Timezone:        "UTC+7",
		Location:        loc,
		DemoMode:        true,
		DefaultBilling:  config.Load().DefaultBilling,
		SSOEnabled:      false,
	}

	store := &memoryStore{}
	h, err := NewServer(cfg, store)
	if err != nil {
		t.Fatalf("NewServer failed: %v", err)
	}

	// 1. Health endpoint
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"ok":true`) {
		t.Fatalf("unexpected /healthz response: %d %s", rec.Code, rec.Body.String())
	}

	// 2. Index HTML renders
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "MiAcStats") {
		t.Fatalf("unexpected / response: %d", rec.Code)
	}

	// 3. Stats query with populated memory store
	ts := time.Date(2026, 11, 11, 22, 0, 0, 0, loc).UTC().Unix()
	store.buckets = append(store.buckets, model.Bucket{
		DeviceID:        "miac-main",
		TsUTC:           ts,
		SessionStartUTC: ts,
		DurationSec:     60,
		CoolSec:         60,
		FanLowSec:       0,
		FanHighSec:      0,
		AutoSec:         60,
	})

	statsRec := httptest.NewRecorder()
	h.ServeHTTP(statsRec, httptest.NewRequest(http.MethodGet, "/api/stats?from=2026-11-11T20:00&to=2026-11-11T23:59", nil))
	if statsRec.Code != http.StatusOK {
		t.Fatalf("expected 200 on /api/stats, got %d %s", statsRec.Code, statsRec.Body.String())
	}
	var dash model.DashboardStats
	if err := json.Unmarshal(statsRec.Body.Bytes(), &dash); err != nil {
		t.Fatalf("unmarshal stats: %v", err)
	}
	if dash.Summary.BucketCount != 1 {
		t.Fatalf("expected 1 bucket in stats, got %d", dash.Summary.BucketCount)
	}
	if len(dash.Sessions) != 1 {
		t.Fatalf("expected 1 session, got %d", len(dash.Sessions))
	}
	if len(dash.Daily) != 1 {
		t.Fatalf("expected 1 daily record, got %d", len(dash.Daily))
	}

	// 4. Settings validation & persistence
	settingsReq := httptest.NewRequest(http.MethodPost, "/api/settings", bytes.NewBufferString(`{"cool_watts":700,"fan_low_watts":30,"fan_high_watts":50,"tariff_mode":"flat","flat_rate_vnd":3200,"vat_percent":8,"base_home_kwh":0}`))
	settingsRec := httptest.NewRecorder()
	h.ServeHTTP(settingsRec, settingsReq)
	if settingsRec.Code != http.StatusOK || store.settings.CoolWatts != 700 {
		t.Fatalf("settings update failed: %d %s", settingsRec.Code, settingsRec.Body.String())
	}
}

type testKeyFetcher struct {
	key *rsa.PublicKey
}

func (f *testKeyFetcher) GetPublicKey(ctx context.Context, kid string) (*rsa.PublicKey, error) {
	return f.key, nil
}

func TestSSOAuthWorkflow(t *testing.T) {
	privKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("generate rsa key: %v", err)
	}

	keyFetcher := &testKeyFetcher{key: &privKey.PublicKey}

	makeTestJWT := func(permissions []string) string {
		hJSON, _ := json.Marshal(map[string]any{"alg": "RS256", "kid": "k1", "typ": "JWT"})
		pJSON, _ := json.Marshal(map[string]any{
			"sub":         "admin-user",
			"permissions": permissions,
			"exp":         time.Now().Add(1 * time.Hour).Unix(),
		})
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

	var mockTokenResp string
	var lastReceivedSecret string
	mockOAuthServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/connect/token" {
			_ = r.ParseForm()
			lastReceivedSecret = r.FormValue("client_secret")
			w.Header().Set("Content-Type", "application/json")
			fmt.Fprint(w, mockTokenResp)
			return
		}
		http.NotFound(w, r)
	}))
	defer mockOAuthServer.Close()

	idClient := auth.NewIdentityClient(mockOAuthServer.URL, "QuickBite_Portal", "test-secret-val", "openid profile", mockOAuthServer.Client())

	cfg := config.Config{
		Port:            "8080",
		SSOEnabled:      true,
		SSOIdentityURL:  mockOAuthServer.URL,
		SSOClientID:     "QuickBite_Portal",
		SSOClientSecret: "test-secret-val",
		SessionSecret:   "test-secret-12345678901234567890",
		SessionMaxAge:   14 * 24 * 3600,
		SecureCookie:   false, // false in test environment
		DefaultBilling: config.Load().DefaultBilling,
	}

	h, err := NewServerWithAuth(cfg, &memoryStore{}, idClient, keyFetcher)
	if err != nil {
		t.Fatalf("NewServerWithAuth failed: %v", err)
	}

	// 1. Unauthenticated GET / must redirect to /login
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	if rec.Code != http.StatusFound || rec.Header().Get("Location") != "/login" {
		t.Fatalf("expected redirect to /login, got code=%d loc=%s", rec.Code, rec.Header().Get("Location"))
	}

	// 2. Unauthenticated GET /api/stats must return 401 JSON
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/api/stats", nil))
	if rec.Code != http.StatusUnauthorized || !strings.Contains(rec.Body.String(), `"unauthorized"`) {
		t.Fatalf("expected 401 unauthorized on /api/stats, got %d %s", rec.Code, rec.Body.String())
	}

	// 3. GET /login must render login page (200 OK)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/login", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "Đăng Nhập") {
		t.Fatalf("expected 200 login page, got %d", rec.Code)
	}

	// 4. POST /login with account missing required permission
	// MUST return generic error "Tên đăng nhập hoặc mật khẩu không đúng." (obscures existence)
	badToken := makeTestJWT([]string{"Order.Orders.AdminView", "AbpIdentity.Users"})
	mockTokenResp = fmt.Sprintf(`{"access_token":%q,"token_type":"Bearer","expires_in":3600,"refresh_token":"rt-1"}`, badToken)

	form := url.Values{}
	form.Set("username", "otheruser")
	form.Set("password", "p@ssword")
	req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 on missing permission, got %d", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), "Tên đăng nhập hoặc mật khẩu không đúng.") {
		t.Fatalf("expected generic error message, got: %s", rec.Body.String())
	}
	if len(rec.Result().Cookies()) > 0 {
		for _, c := range rec.Result().Cookies() {
			if c.Name == auth.SessionCookieName && c.Value != "" {
				t.Fatalf("cookie must not be set on failed permission check")
			}
		}
	}

	// 5. POST /login with valid permission "xiaomi-nwt-airconditioner"
	validToken := makeTestJWT([]string{"xiaomi-nwt-airconditioner", "AbpIdentity.Users"})
	mockTokenResp = fmt.Sprintf(`{"access_token":%q,"token_type":"Bearer","expires_in":3600,"refresh_token":"rt-valid"}`, validToken)

	form = url.Values{}
	form.Set("username", "validuser")
	form.Set("password", "correctpass")
	req = httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if lastReceivedSecret != "test-secret-val" {
		t.Errorf("expected client_secret 'test-secret-val' sent to token endpoint, got %q", lastReceivedSecret)
	}

	if rec.Code != http.StatusSeeOther || rec.Header().Get("Location") != "/" {
		t.Fatalf("expected 303 redirect to /, got code=%d loc=%s", rec.Code, rec.Header().Get("Location"))
	}

	var sessionCookie *http.Cookie
	for _, c := range rec.Result().Cookies() {
		if c.Name == auth.SessionCookieName && c.Value != "" {
			sessionCookie = c
			break
		}
	}
	if sessionCookie == nil {
		t.Fatalf("expected mia_session cookie to be set")
	}
	if !sessionCookie.HttpOnly {
		t.Errorf("expected session cookie to be HttpOnly")
	}

	// 6. Access protected route with the session cookie -> must succeed (200 OK)
	authReq := httptest.NewRequest(http.MethodGet, "/", nil)
	authReq.AddCookie(sessionCookie)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, authReq)

	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "MiAcStats") {
		t.Fatalf("expected 200 OK dashboard with cookie, got %d", rec.Code)
	}

	// 7. Access /login while logged in -> should redirect to /
	loginReq := httptest.NewRequest(http.MethodGet, "/login", nil)
	loginReq.AddCookie(sessionCookie)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, loginReq)
	if rec.Code != http.StatusFound || rec.Header().Get("Location") != "/" {
		t.Fatalf("expected redirect to / when already authenticated, got %d %s", rec.Code, rec.Header().Get("Location"))
	}

	// 8. POST /logout -> should clear session cookie and redirect to /login
	logoutReq := httptest.NewRequest(http.MethodPost, "/logout", nil)
	logoutReq.AddCookie(sessionCookie)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, logoutReq)

	if rec.Code != http.StatusSeeOther || rec.Header().Get("Location") != "/login" {
		t.Fatalf("expected 303 redirect to /login on logout, got %d %s", rec.Code, rec.Header().Get("Location"))
	}

	logoutCookie := rec.Result().Cookies()
	var cleared bool
	for _, c := range logoutCookie {
		if c.Name == auth.SessionCookieName && (c.MaxAge < 0 || c.Value == "") {
			cleared = true
		}
	}
	if !cleared {
		t.Fatalf("expected session cookie to be cleared on logout")
	}
}
