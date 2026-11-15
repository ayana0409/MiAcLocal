package auth

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"sync"
	"time"
)

// SessionCookieName is the standard cookie name for the encrypted session.
const SessionCookieName = "mia_session"

// SessionData holds the authenticated credentials stored in the server-side session store.
type SessionData struct {
	SessionID      string    `json:"sid"`
	AccessToken    string    `json:"at"`
	RefreshToken   string    `json:"rt,omitempty"`
	IDToken        string    `json:"it,omitempty"`
	Username       string    `json:"un,omitempty"`
	TokenExpiresAt time.Time `json:"tea"`
	ExpiresAt      time.Time `json:"exp"`
}

// SessionStore manages in-memory sessions with concurrent-safe access and TTL expiration.
type SessionStore struct {
	mu       sync.RWMutex
	sessions map[string]*SessionData
}

// NewSessionStore creates a new in-memory session store with background cleanup.
func NewSessionStore() *SessionStore {
	store := &SessionStore{
		sessions: make(map[string]*SessionData),
	}
	go store.cleanupLoop(5 * time.Minute)
	return store
}

// Create generates a secure 32-byte hex session ID, stores the session data, and returns the ID.
func (s *SessionStore) Create(data *SessionData) (string, error) {
	b := make([]byte, 32)
	if _, err := io.ReadFull(rand.Reader, b); err != nil {
		return "", err
	}
	sid := hex.EncodeToString(b)
	data.SessionID = sid

	s.mu.Lock()
	defer s.mu.Unlock()
	s.sessions[sid] = data
	return sid, nil
}

// Get retrieves a session by its ID, returning nil and false if not found or expired.
func (s *SessionStore) Get(sid string) (*SessionData, bool) {
	if sid == "" {
		return nil, false
	}
	s.mu.RLock()
	data, exists := s.sessions[sid]
	s.mu.RUnlock()
	if !exists {
		return nil, false
	}
	if !data.ExpiresAt.IsZero() && time.Now().After(data.ExpiresAt) {
		s.Delete(sid)
		return nil, false
	}
	return data, true
}

// Update updates an existing session in the store.
func (s *SessionStore) Update(sid string, data *SessionData) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.sessions[sid] = data
}

// Delete removes a session from the store.
func (s *SessionStore) Delete(sid string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.sessions, sid)
}

func (s *SessionStore) cleanupLoop(interval time.Duration) {
	ticker := time.NewTicker(interval)
	for range ticker.C {
		s.cleanupExpired()
	}
}

func (s *SessionStore) cleanupExpired() {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	for sid, data := range s.sessions {
		if !data.ExpiresAt.IsZero() && now.After(data.ExpiresAt) {
			delete(s.sessions, sid)
		}
	}
}

// DeriveKey turns any arbitrary string into a 32-byte key for AES-256 using SHA-256.
func DeriveKey(secret string) []byte {
	hash := sha256.Sum256([]byte(secret))
	return hash[:]
}

// EncryptSessionID encrypts a session ID using AES-256-GCM.
func EncryptSessionID(sessionID string, key []byte) (string, error) {
	block, err := aes.NewCipher(key)
	if err != nil {
		return "", err
	}

	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return "", err
	}

	nonce := make([]byte, gcm.NonceSize())
	if _, err := io.ReadFull(rand.Reader, nonce); err != nil {
		return "", err
	}

	ciphertext := gcm.Seal(nonce, nonce, []byte(sessionID), nil)
	return base64.RawURLEncoding.EncodeToString(ciphertext), nil
}

// DecryptSessionID decodes and decrypts an encrypted session ID using AES-256-GCM.
func DecryptSessionID(encrypted string, key []byte) (string, error) {
	raw, err := base64.RawURLEncoding.DecodeString(encrypted)
	if err != nil {
		return "", err
	}

	block, err := aes.NewCipher(key)
	if err != nil {
		return "", err
	}

	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return "", err
	}

	nonceSize := gcm.NonceSize()
	if len(raw) < nonceSize {
		return "", errors.New("malformed session cookie")
	}

	nonce, ciphertext := raw[:nonceSize], raw[nonceSize:]
	plain, err := gcm.Open(nil, nonce, ciphertext, nil)
	if err != nil {
		return "", err
	}

	return string(plain), nil
}

// SetSessionCookie sets the encrypted HTTP-only session ID cookie.
// Payload is lightweight (~120 bytes), guaranteed to fit well below the RFC 6265 4KB limit.
func SetSessionCookie(w http.ResponseWriter, sessionID string, key []byte, secure bool, maxAgeSec int) error {
	encrypted, err := EncryptSessionID(sessionID, key)
	if err != nil {
		return err
	}

	cookie := &http.Cookie{
		Name:     SessionCookieName,
		Value:    encrypted,
		Path:     "/",
		MaxAge:   maxAgeSec,
		HttpOnly: true,
		Secure:   secure,
		SameSite: http.SameSiteLaxMode,
	}
	http.SetCookie(w, cookie)
	return nil
}

// GetSessionID reads and decrypts the session ID from the request cookie.
func GetSessionID(r *http.Request, key []byte) (string, error) {
	cookie, err := r.Cookie(SessionCookieName)
	if err != nil {
		return "", err
	}
	if cookie.Value == "" {
		return "", errors.New("empty session cookie")
	}
	return DecryptSessionID(cookie.Value, key)
}

// ClearSessionCookie deletes the session cookie from the browser.
func ClearSessionCookie(w http.ResponseWriter, secure bool) {
	cookie := &http.Cookie{
		Name:     SessionCookieName,
		Value:    "",
		Path:     "/",
		MaxAge:   -1,
		Expires:  time.Unix(0, 0),
		HttpOnly: true,
		Secure:   secure,
		SameSite: http.SameSiteLaxMode,
	}
	http.SetCookie(w, cookie)
}
