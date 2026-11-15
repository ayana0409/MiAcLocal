package web

import (
	"context"
	"embed"
	"encoding/json"
	"errors"
	"html/template"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"

	"miacstats/internal/auth"
	"miacstats/internal/config"
	"miacstats/internal/model"
	"miacstats/internal/stats"
	"miacstats/internal/turso"
)

//go:embed templates/index.html templates/login.html
var embeddedAssets embed.FS

const genericLoginError = "Tên đăng nhập hoặc mật khẩu không đúng."

// Server handles HTTP endpoints for MiAcStats dashboard and JSON APIs.
type Server struct {
	cfg         config.Config
	store       turso.Store
	indexHTML   []byte
	loginTmpl   *template.Template
	mu          sync.RWMutex
	memSettings model.BillingSettings
	nowFn       func() time.Time

	// SSO Auth dependencies
	idClient     *auth.IdentityClient
	jwksCache    auth.KeyFetcher
	sessionKey   []byte
	sessionStore *auth.SessionStore
}

// NewServer constructs an HTTP handler for the MiAcStats service.
func NewServer(cfg config.Config, store turso.Store) (http.Handler, error) {
	return NewServerWithAuth(cfg, store, nil, nil)
}

// NewServerWithAuth allows injecting custom OAuth and JWKS clients (used for testing).
func NewServerWithAuth(cfg config.Config, store turso.Store, idClient *auth.IdentityClient, jwksCache auth.KeyFetcher) (http.Handler, error) {
	htmlBytes, err := embeddedAssets.ReadFile("templates/index.html")
	if err != nil {
		return nil, err
	}

	loginHTMLBytes, err := embeddedAssets.ReadFile("templates/login.html")
	if err != nil {
		return nil, err
	}

	tmpl, err := template.New("login").Parse(string(loginHTMLBytes))
	if err != nil {
		return nil, err
	}

	if idClient == nil && cfg.SSOEnabled {
		idClient = auth.NewIdentityClient(cfg.SSOIdentityURL, cfg.SSOClientID, cfg.SSOClientSecret, cfg.SSOScope, nil)
	}
	if jwksCache == nil && cfg.SSOEnabled {
		jwksURL := strings.TrimRight(cfg.SSOIdentityURL, "/") + "/.well-known/jwks"
		jwksCache = auth.NewJWKSCache(jwksURL, nil)
	}

	s := &Server{
		cfg:          cfg,
		store:        store,
		indexHTML:    htmlBytes,
		loginTmpl:    tmpl,
		memSettings:  cfg.DefaultBilling,
		nowFn:        time.Now,
		idClient:     idClient,
		jwksCache:    jwksCache,
		sessionKey:   auth.DeriveKey(cfg.SessionSecret),
		sessionStore: auth.NewSessionStore(),
	}

	mux := http.NewServeMux()

	// Public endpoints (no authentication required)
	mux.HandleFunc("GET /healthz", s.handleHealth)
	mux.HandleFunc("GET /login", s.handleLoginPage)
	mux.HandleFunc("POST /login", s.handleLoginPost)
	mux.HandleFunc("POST /logout", s.handleLogout)

	// Protected endpoints (guarded by authMiddleware)
	protectedMux := http.NewServeMux()
	protectedMux.HandleFunc("GET /{$}", s.handleIndex)
	protectedMux.HandleFunc("GET /api/stats", s.handleGetStats)
	protectedMux.HandleFunc("POST /api/settings", s.handlePostSettings)

	mux.Handle("/", s.authMiddleware(protectedMux))

	return mux, nil
}

func (s *Server) isSecureCookie(r *http.Request) bool {
	if !s.cfg.SecureCookie {
		return false
	}
	// If the connection is directly TLS, or behind a reverse proxy terminating HTTPS
	if r.TLS != nil || strings.EqualFold(r.Header.Get("X-Forwarded-Proto"), "https") {
		return true
	}
	return false
}

func (s *Server) authMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !s.cfg.SSOEnabled {
			next.ServeHTTP(w, r)
			return
		}

		sid, err := auth.GetSessionID(r, s.sessionKey)
		if err != nil || sid == "" {
			if err != nil && !errors.Is(err, http.ErrNoCookie) {
				log.Printf("authMiddleware: session cookie read/decrypt error on %s: %v", r.URL.Path, err)
			} else {
				log.Printf("authMiddleware: unauthenticated request to %s (no session cookie)", r.URL.Path)
			}
			s.handleUnauthorized(w, r)
			return
		}

		sess, ok := s.sessionStore.Get(sid)
		if !ok || sess == nil || sess.AccessToken == "" {
			log.Printf("authMiddleware: session not found or expired for %s", r.URL.Path)
			auth.ClearSessionCookie(w, s.isSecureCookie(r))
			s.handleUnauthorized(w, r)
			return
		}

		ctx := r.Context()
		now := s.nowFn()

		// If access token is near expiry (< 60s) or already expired, attempt refresh
		if !sess.TokenExpiresAt.IsZero() && sess.TokenExpiresAt.Sub(now) < 60*time.Second && sess.RefreshToken != "" && s.idClient != nil {
			refreshCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
			tr, refErr := s.idClient.RefreshToken(refreshCtx, sess.RefreshToken)
			cancel()
			if refErr == nil && tr != nil && tr.AccessToken != "" {
				sess.AccessToken = tr.AccessToken
				if tr.RefreshToken != "" {
					sess.RefreshToken = tr.RefreshToken
				}
				sess.TokenExpiresAt = now.Add(time.Duration(tr.ExpiresIn) * time.Second)
				s.sessionStore.Update(sid, sess)
				log.Printf("authMiddleware: refreshed access token for user %q", sess.Username)
			} else {
				log.Printf("authMiddleware: token refresh failed for user %q: %v", sess.Username, refErr)
			}
		}

		// Verify signature and check permissions
		var claims map[string]any
		if s.jwksCache != nil {
			claims, err = auth.VerifyRS256(ctx, sess.AccessToken, s.jwksCache)
			if err != nil {
				// If verification fails, try refreshing once if we haven't already
				if sess.RefreshToken != "" && s.idClient != nil {
					refreshCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
					tr, refErr := s.idClient.RefreshToken(refreshCtx, sess.RefreshToken)
					cancel()
					if refErr == nil && tr != nil && tr.AccessToken != "" {
						sess.AccessToken = tr.AccessToken
						if tr.RefreshToken != "" {
							sess.RefreshToken = tr.RefreshToken
						}
						sess.TokenExpiresAt = now.Add(time.Duration(tr.ExpiresIn) * time.Second)
						s.sessionStore.Update(sid, sess)
						claims, err = auth.VerifyRS256(ctx, sess.AccessToken, s.jwksCache)
					}
				}
			}
		} else {
			claims, err = auth.ParseUnverifiedClaims(sess.AccessToken)
		}

		if err != nil || claims == nil {
			log.Printf("authMiddleware: token verify error for user %q: %v", sess.Username, err)
			s.sessionStore.Delete(sid)
			auth.ClearSessionCookie(w, s.isSecureCookie(r))
			s.handleUnauthorized(w, r)
			return
		}

		hasPerm := auth.HasRequiredPermission(claims)
		if !hasPerm && sess.IDToken != "" {
			if idClaims, err := auth.ParseUnverifiedClaims(sess.IDToken); err == nil {
				hasPerm = auth.HasRequiredPermission(idClaims)
			}
		}

		if !hasPerm {
			log.Printf("authMiddleware: user %q lacks permission %q", sess.Username, auth.RequiredPermission)
			s.sessionStore.Delete(sid)
			auth.ClearSessionCookie(w, s.isSecureCookie(r))
			s.handleUnauthorized(w, r)
			return
		}

		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleUnauthorized(w http.ResponseWriter, r *http.Request) {
	if strings.HasPrefix(r.URL.Path, "/api/") {
		writeJSONError(w, http.StatusUnauthorized, "unauthorized")
		return
	}
	http.Redirect(w, r, "/login", http.StatusFound)
}

func (s *Server) handleLoginPage(w http.ResponseWriter, r *http.Request) {
	if s.cfg.SSOEnabled {
		// If user already holds a valid session with required permission, redirect to /
		sid, err := auth.GetSessionID(r, s.sessionKey)
		if err == nil && sid != "" {
			if sess, ok := s.sessionStore.Get(sid); ok && sess != nil && sess.AccessToken != "" {
				claims, cErr := auth.ParseUnverifiedClaims(sess.AccessToken)
				hasPerm := cErr == nil && auth.HasRequiredPermission(claims)
				if !hasPerm && sess.IDToken != "" {
					if idClaims, err := auth.ParseUnverifiedClaims(sess.IDToken); err == nil {
						hasPerm = auth.HasRequiredPermission(idClaims)
					}
				}
				if hasPerm {
					http.Redirect(w, r, "/", http.StatusFound)
					return
				}
			}
		}
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_ = s.loginTmpl.Execute(w, map[string]any{
		"Error":    "",
		"Username": "",
	})
}

func (s *Server) handleLoginPost(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		s.renderLoginError(w, genericLoginError, "")
		return
	}

	username := strings.TrimSpace(r.FormValue("username"))
	password := strings.TrimSpace(r.FormValue("password"))

	if username == "" || password == "" {
		s.renderLoginError(w, genericLoginError, username)
		return
	}

	if !s.cfg.SSOEnabled || s.idClient == nil {
		// If SSO disabled, allow login
		http.Redirect(w, r, "/", http.StatusSeeOther)
		return
	}

	// Request token from QuickBite Identity (Password Grant)
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	tr, err := s.idClient.LoginPassword(ctx, username, password)
	cancel()

	if err != nil || tr == nil || tr.AccessToken == "" {
		log.Printf("login failed for user %q: %v", username, err)
		s.renderLoginError(w, genericLoginError, username)
		return
	}

	// Verify token signature & extract claims
	var claims map[string]any
	if s.jwksCache != nil {
		vCtx, vCancel := context.WithTimeout(r.Context(), 10*time.Second)
		claims, err = auth.VerifyRS256(vCtx, tr.AccessToken, s.jwksCache)
		vCancel()
		if err != nil {
			log.Printf("token verification failed for user %q: %v", username, err)
			s.renderLoginError(w, genericLoginError, username)
			return
		}
	} else {
		claims, err = auth.ParseUnverifiedClaims(tr.AccessToken)
		if err != nil {
			s.renderLoginError(w, genericLoginError, username)
			return
		}
	}

	// CRITICAL: Check required permission "xiaomi-nwt-airconditioner".
	// Check access_token claims, and fall back to id_token claims if available.
	hasPerm := auth.HasRequiredPermission(claims)
	if !hasPerm && tr.IDToken != "" {
		if idClaims, err := auth.ParseUnverifiedClaims(tr.IDToken); err == nil {
			if auth.HasRequiredPermission(idClaims) {
				hasPerm = true
			}
		}
	}

	if !hasPerm {
		claimsJSON, _ := json.Marshal(claims)
		var idClaimsStr string
		if tr.IDToken != "" {
			if idClaims, err := auth.ParseUnverifiedClaims(tr.IDToken); err == nil {
				b, _ := json.Marshal(idClaims)
				idClaimsStr = string(b)
			}
		}
		log.Printf("access denied: user %q lacks permission %q; access_token claims: %s; id_token claims: %s",
			username, auth.RequiredPermission, string(claimsJSON), idClaimsStr)
		s.renderLoginError(w, genericLoginError, username)
		return
	}

	// Authentication & authorization successful
	tokenExp := s.nowFn().Add(time.Duration(tr.ExpiresIn) * time.Second)
	sessExp := s.nowFn().Add(time.Duration(s.cfg.SessionMaxAge) * time.Second)
	sess := &auth.SessionData{
		AccessToken:    tr.AccessToken,
		RefreshToken:   tr.RefreshToken,
		IDToken:        tr.IDToken,
		Username:       username,
		TokenExpiresAt: tokenExp,
		ExpiresAt:      sessExp,
	}

	sid, err := s.sessionStore.Create(sess)
	if err != nil {
		log.Printf("create session failed: %v", err)
		s.renderLoginError(w, "Lỗi tạo phiên đăng nhập.", username)
		return
	}

	isSec := s.isSecureCookie(r)
	if err := auth.SetSessionCookie(w, sid, s.sessionKey, isSec, s.cfg.SessionMaxAge); err != nil {
		log.Printf("set session cookie failed: %v", err)
		s.renderLoginError(w, "Lỗi tạo phiên đăng nhập.", username)
		return
	}

	log.Printf("user %q successfully logged in (sid=%s..., secure_cookie=%v), redirecting to /", username, sid[:8], isSec)
	http.Redirect(w, r, "/", http.StatusSeeOther)
}

func (s *Server) renderLoginError(w http.ResponseWriter, msg, username string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusUnauthorized)
	_ = s.loginTmpl.Execute(w, map[string]any{
		"Error":    msg,
		"Username": username,
	})
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if sid, err := auth.GetSessionID(r, s.sessionKey); err == nil && sid != "" {
		s.sessionStore.Delete(sid)
	}
	auth.ClearSessionCookie(w, s.isSecureCookie(r))
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":        true,
		"demo_mode": s.isDemoActive(),
		"sso":       s.cfg.SSOEnabled,
	})
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(s.indexHTML)
}

func (s *Server) handleGetStats(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	deviceID := strings.TrimSpace(q.Get("device"))
	if deviceID == "" {
		deviceID = s.cfg.DefaultDeviceID
	}

	nowLocal := s.nowFn().In(s.cfg.Location)

	// Default: 1st of current calendar month 00:00 to end of current month 23:59
	startOfMonth := time.Date(nowLocal.Year(), nowLocal.Month(), 1, 0, 0, 0, 0, s.cfg.Location)
	endOfMonth := startOfMonth.AddDate(0, 1, -1)
	endOfMonth = time.Date(endOfMonth.Year(), endOfMonth.Month(), endOfMonth.Day(), 23, 59, 59, 0, s.cfg.Location)

	fromRaw := strings.TrimSpace(q.Get("from"))
	toRaw := strings.TrimSpace(q.Get("to"))

	fromTime := startOfMonth
	if fromRaw != "" {
		parsed, err := parseDateTime(fromRaw, s.cfg.Location, false)
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, "invalid 'from' date/time (use YYYY-MM-DD or YYYY-MM-DDTHH:mm)")
			return
		}
		fromTime = parsed
	}

	toTime := endOfMonth
	if toRaw != "" {
		parsed, err := parseDateTime(toRaw, s.cfg.Location, true)
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, "invalid 'to' date/time (use YYYY-MM-DD or YYYY-MM-DDTHH:mm)")
			return
		}
		toTime = parsed
	}

	if toTime.Before(fromTime) {
		writeJSONError(w, http.StatusBadRequest, "'to' date/time must not precede 'from' date/time")
		return
	}
	if toTime.Sub(fromTime) > 366*24*time.Hour {
		writeJSONError(w, http.StatusBadRequest, "date range cannot exceed 366 days")
		return
	}

	fromUTC := fromTime.UTC().Unix()
	toUTC := toTime.UTC().Unix()

	billing := s.currentDefaults()
	var buckets []model.Bucket

	if s.store != nil {
		loadedCfg, err := s.store.LoadBillingSettings(r.Context(), billing)
		if err == nil {
			billing = stats.NormalizeSettings(loadedCfg, s.cfg.DefaultBilling)
		}
		buckets, err = s.store.QueryBuckets(r.Context(), deviceID, fromUTC, toUTC)
		if err != nil {
			writeJSONError(w, http.StatusBadGateway, "failed to query Turso: "+err.Error())
			return
		}
	} else if s.cfg.DemoMode {
		allDemo := stats.DemoBuckets(s.nowFn(), s.cfg.Location, deviceID)
		for _, b := range allDemo {
			if b.TsUTC >= fromUTC && b.TsUTC <= toUTC {
				buckets = append(buckets, b)
			}
		}
	}

	dash := stats.ComputeDashboard(
		deviceID,
		fromTime.Format("2006-01-02T15:04"),
		toTime.Format("2006-01-02T15:04"),
		s.cfg.Location,
		s.isDemoActive(),
		buckets,
		billing,
	)
	writeJSON(w, http.StatusOK, dash)
}

func (s *Server) handlePostSettings(w http.ResponseWriter, r *http.Request) {
	var in model.BillingSettings
	if err := decodeStrictJSON(r, &in); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if in.CoolWatts <= 0 || in.FanLowWatts < 0 || in.FanHighWatts < 0 {
		writeJSONError(w, http.StatusBadRequest, "cool_watts must be positive, fan watts must be non-negative")
		return
	}
	if in.TariffMode != "evn" && in.TariffMode != "flat" {
		writeJSONError(w, http.StatusBadRequest, "tariff_mode must be 'evn' or 'flat'")
		return
	}
	if in.VATPercent < 0 || in.VATPercent > 100 || in.FlatRateVND < 0 || in.BaseHomeKWh < 0 {
		writeJSONError(w, http.StatusBadRequest, "invalid rate, base kWh, or VAT percentage")
		return
	}

	cleaned := stats.NormalizeSettings(in, s.cfg.DefaultBilling)
	if s.store != nil {
		if err := s.store.SaveBillingSettings(r.Context(), cleaned); err != nil {
			writeJSONError(w, http.StatusBadGateway, "failed to save settings to Turso: "+err.Error())
			return
		}
	}

	s.mu.Lock()
	s.memSettings = cleaned
	s.mu.Unlock()

	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"settings": cleaned,
	})
}

func (s *Server) currentDefaults() model.BillingSettings {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.memSettings
}

func (s *Server) isDemoActive() bool {
	return s.store == nil && s.cfg.DemoMode
}

func parseDateTime(raw string, loc *time.Location, isEnd bool) (time.Time, error) {
	raw = strings.TrimSpace(raw)
	formats := []string{
		"2006-01-02T15:04:05",
		"2006-01-02T15:04",
		"2006-01-02 15:04",
		"2006-01-02",
	}
	for _, fmtStr := range formats {
		if t, err := time.ParseInLocation(fmtStr, raw, loc); err == nil {
			if fmtStr == "2006-01-02" && isEnd {
				return time.Date(t.Year(), t.Month(), t.Day(), 23, 59, 59, 0, loc), nil
			}
			return t, nil
		}
	}
	return time.Time{}, errors.New("cannot parse datetime")
}

func decodeStrictJSON(r *http.Request, dst any) error {
	r.Body = http.MaxBytesReader(nil, r.Body, 1<<20)
	dec := json.NewDecoder(r.Body)
	if err := dec.Decode(dst); err != nil {
		return errors.New("invalid JSON payload")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeJSONError(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]any{
		"ok":    false,
		"error": msg,
	})
}
