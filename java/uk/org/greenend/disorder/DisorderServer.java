/*
 * This file is part of DisOrder.
 * Copyright (C) 2010 Richard Kettlewell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
package uk.org.greenend.disorder;

import java.io.*;
import java.net.*;
import java.util.*;
import java.security.*;
import java.nio.charset.*;

/**
 * A synchronous connection to a DisOrder server.
 *
 * <p>Having created a connection, use the various methods to play
 * tracks, list the queue, etc.  Most methods have exactly the same
 * name as the underlying protocol command, the exceptions being where
 * there is a clash with a Java reserved word.
 *
 * <p><b>This is a work in progress</b>. Many commands are not
 * implemented.
 *
 * <p>Commands that require admin rights are not implemented as they
 * only work on local connections, and this class always uses a TCP
 * connection.
 */
public class DisorderServer {
  private DisorderConfig config;
  private boolean connected;
  private Socket conn;
  private BufferedReader input;
  private PrintWriter output;
  private boolean debugging;
  private Formatter formatter;

  /**
   * Construct a server connection from a given configuration.
   *
   * <p>Creating a connection does not connect it.  Instead the
   * underlying connection is established either on demand or via the
   * {@link #connect() connect} method.
   *
   * @param c Configuration to use
   */
  public DisorderServer(DisorderConfig c) {
    if(System.getProperty("DISORDER_DEBUG") != null)
      debugging = true;
    config = c;
    connected = false;
  }

  /**
   * Construct a server connection from the default configuration.
   *
   * <p>Creating a connection does not connect it.  Instead the
   * underlying connection is established either on demand or via the
   * {@link #connect() connect} method.
   *
   * @throws DisorderParseError If a configuration file contains a syntax error
   * @throws IOException If an error occurs reading a configuration file
   */
  public DisorderServer() throws DisorderParseError,
                                 IOException {
    if(System.getProperty("DISORDER_DEBUG") != null)
      debugging = true;
    config = new DisorderConfig();
    connected = false;
  }

  /** Connect to the server.
   *
   * <p>Establishes a TCP connection to the server and authenticates
   * using the username and password from the configuration chosen at
   * construction time.
   *
   * <p>Does nothing if already connected.
   *
   * <p>Note that it's never <i>necessary</i> to call this method.
   * The other methods will call it automatically.  However, if you
   * want to distinguish connection and authentication errors from
   * errors occurring later, it may be convenient to do so.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void connect() throws IOException,
                               DisorderParseError,
                               DisorderProtocolError {
    if(connected)
      return;
    conn = new Socket(config.serverName, config.serverPort);
    input = new BufferedReader(
		new InputStreamReader(conn.getInputStream(),
				      Charset.forName("UTF-8")));
    output = new PrintWriter(
		new BufferedWriter(
		    new OutputStreamWriter(conn.getOutputStream(),
					   Charset.forName("UTF-8"))));
    // Field the initial greeting and attempt to log in
    {
      String greeting = getResponse();
      Vector<String> v = DisorderMisc.split(greeting, false/*comments*/);
      int rc = responseCode(v);
      if(rc != 231)
        throw new DisorderProtocolError(config.serverName, greeting);
      if(!v.get(1).equals("2"))
        throw new DisorderParseError("unknown protocol generation: " + v.get(1));
      String alg = v.get(2);
      String challenge = v.get(3);
      // Identify the hashing algorithm to use
      MessageDigest md;
      try {
        if(alg.equalsIgnoreCase("sha1"))
          md = MessageDigest.getInstance("SHA-1");
        else if(alg.equalsIgnoreCase("sha256"))
          md = MessageDigest.getInstance("SHA-256");
        else if(alg.equalsIgnoreCase("sha384"))
          md = MessageDigest.getInstance("SHA-384");
        else if(alg.equalsIgnoreCase("sha512"))
          md = MessageDigest.getInstance("SHA-512");
        else
          throw new DisorderParseError("unknown authentication algorithm: " + alg);
      } catch(NoSuchAlgorithmException e) {
        throw new RuntimeException("message digest " + alg + " not available", e);
      }
      // Construct the response
      md.update(config.password.getBytes());
      md.update(DisorderMisc.fromHex(challenge));
      String response = DisorderMisc.toHex(md.digest());
      send("user %s %s", config.user, response);
    }
    // Handle the response to our login attempt
    {
      String response = getResponse();
      Vector<String> v = DisorderMisc.split(response, false/*comments*/);
      int rc = responseCode(v);
      if(rc / 100 != 2)
        throw new DisorderProtocolError(config.serverName,
                                        "authentication failed: " + response);
    }
    connected = true;
  }

  /**
   * Send a command to the server.
   *
   * A newline is appended to the command automatically.
   *
   * @param format Format string
   * @param args Arguments to <code>format</code>
   */
  private void send(String format, Object ... args) {
    if(formatter == null)
      formatter = new Formatter();
    formatter.format(format, args);
    StringBuilder sb = (StringBuilder)formatter.out();
    String s = sb.toString();
    if(debugging) {
      System.err.print("SEND: ");
      System.err.println(s);
    }
    output.print(s);
    output.print('\n');         // no, not println!
    output.flush();
    sb.delete(0, s.length());
  }

  /**
   * Read a response form the server.
   *
   * Reads one line from the server and returns it as a string.  Note
   * that the trailing newline character is not included.
   *
   * @return Line from server, or null at end of input.
   * @throws IOException If a network IO error occurs
   */
  private String getResponse() throws IOException {
    StringBuilder sb = new StringBuilder();
    int n;
    while((n = input.read()) != -1) {
      char c = (char)n;
      if(c == '\n') {
        String s = sb.toString();
        if(debugging) 
          System.err.println("RECV: " + s);
        return s;
      }
      sb.append(c);
    }
    // We reached end of file
    return null;
  }

  /**
   * Pick out a response code.
   *
   * @return Response code in the range 0-999.
   * @throws DisorderParseError If the response is malformed
   */
  private int responseCode(List<String> v) throws DisorderParseError {
    if(v.size() < 1)
      throw new DisorderParseError("empty response");
    int rc = Integer.parseInt(v.get(0));
    if(rc < 0 || rc > 999)
      throw new DisorderParseError("invalid response code " + v.get(0));
    return rc;
  }

  /**
   * Await a response and check it's OK.
   *
   * Any 2xx response counts as success.  Anything else will generate
   * a DisorderProtocolError.
   *
   * @return Split response
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  private Vector<String> getPositiveResponse() throws IOException,
                                                      DisorderParseError,
                                                      DisorderProtocolError {
    String response = getResponse();
    Vector<String> v = DisorderMisc.split(response, false/*comments*/);
    int rc = responseCode(v);
    if(rc / 100 != 2)
      throw new DisorderProtocolError(config.serverName, response);
    return v;
  }

  /**
   * Get a string response
   *
   * <p>Any 2xx response with exactly one extra field will return the
   * value of that field.
   *
   * <p>If <code>optional<code> is <code>true</code> then a 555
   * response is also accepted, with a return value of
   * <code>null</code>.
   *
   * <p>Otherwise there will be a DisorderProtocolError or
   * DisorderParseError.
   *
   * @param optional Whether 555 responses are acceptable
   * @return Response value or <code>null</code>
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  String getStringResponse(boolean optional) throws IOException,
                                                    DisorderParseError,
                                                    DisorderProtocolError {
    String response = getResponse();
    Vector<String> v = DisorderMisc.split(response, false/*comments*/);
    int rc = responseCode(v);
    if(optional && rc == 555)
      return null;
    if(rc / 100 != 2)
      throw new DisorderProtocolError(config.serverName, response);
    if(v.size() != 2)
      throw new DisorderParseError("malformed response: " + response);
    return v.get(1);
  }

  /**
   * Get a boolean response
   *
   * Any 2xx response with the next field being "yes" or "no" counts
   * as success.  Anything else will generate a DisorderProtocolError
   * or DisorderParseError.
   *
   * @return Response truth value
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  boolean getBooleanResponse() throws IOException,
                                      DisorderParseError,
                                      DisorderProtocolError {
    String s = getStringResponse(false);
    if(s.equals("yes"))
      return true;
    else if(s.equals("no"))
      return false;
    else
      throw new DisorderParseError("expxected 'yes' or 'no' in response");
  }

  /**
   * Quote a string for use in a Disorder command
   *
   * @param s String to quote
   * @return Quoted string (possibly the same object as <code>s</code>)
   */
  static private String quote(String s) {
    if(s.length() == 0 || s.matches("[^\"\'\\#\0-\37 ]")) {
      StringBuilder sb = new StringBuilder();
      sb.append('"');
      for(int n = 0; n < s.length(); ++n) {
        char c = s.charAt(n);
        switch(c) {
        case '"':
        case '\\':
          sb.append('\\');
          // fall through
        default:
          sb.append(c);
          break;
        case '\n':
          sb.append('\\');
          sb.append('\n');
          break;
        }
      }
      sb.append('"');
      return sb.toString();
    } else
      return s;
  }

  /**
   * Get a response body.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   */
  List<String> getResponseBody() throws IOException,
                                        DisorderParseError {
    Vector<String> r = new Vector<String>();
    String s;
    while((s = getResponse()) != null
          && !s.equals("."))
      if(s.length() > 0 && s.charAt(0) == '.')
        r.add(s.substring(1));
      else
        r.add(s);
    if(s == null)
      throw new DisorderParseError("unterminated response body");
    return r;
  }

  // TODO adduser not implemented because only works on local connections

  /**
   * Adopt a track.
   *
   * Adopts a randomly picked track, leaving it in the same state as
   * if it was picked by the calling user.
   *
   * @param id Track to adopt
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void adopt(String id) throws IOException,
                                      DisorderParseError,
                                      DisorderProtocolError {
    connect();
    send("adopt %s", quote(id));
    getPositiveResponse();
  }

  /**
   * Get a list of files and directories.
   *
   * Returns a list of the files and directories immediately below
   * <code>path</code>.  If <code>regexp</code> is not
   * <code>null</code> then only matching names are returned.
   *
   * <p>If you need to tell files and directories apart, use {@link
   * #dirs(String,String) dirs()} and {@link #files(String,String)
   * files()} instead.
   *
   * @param path Parent directory
   * @param regexp Regular expression to filter results, or <code>null</code>
   * @return List of files and directiories
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> allfiles(String path,
                                 String regexp) throws IOException,
                                                       DisorderParseError,
                                                       DisorderProtocolError {
    connect();
    if(regexp != null)
      send("allfiles %s %s", quote(path), quote(regexp));
    else
      send("allfiles %s", quote(path));
    getPositiveResponse();
    return getResponseBody();
  }

  // TODO confirm not implemented because only used by CGI
  // TODO cookie not implemented because only used by CGI
  // TODO deluser not implemented because only works on local connections

  /**
   * Get a list of directories.
   *
   * Returns a list of the directories immediately below
   * <code>path</code>.  If <code>regexp</code> is not
   * <code>null</code> then only matching names are returned.
   *
   * @param path Parent directory
   * @param regexp Regular expression to filter results, or <code>null</code>
   * @return List of directories
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> dirs(String path,
                             String regexp) throws IOException,
                                                   DisorderParseError,
                                                   DisorderProtocolError {
    connect();
    if(regexp != null)
      send("dirs %s %s", quote(path), quote(regexp));
    else
      send("dirs %s", quote(path));
    getPositiveResponse();
    return getResponseBody();
  }

  /**
   * Disable further playing.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void disable() throws IOException,
                               DisorderParseError,
                               DisorderProtocolError {
    connect();
    send("disable");
    getPositiveResponse();
  }

  /**
   * Set a user property.
   *
   * @param username User to edit
   * @param property Property to set
   * @param value New value for property
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void edituser(String username,
                       String property,
                       String value) throws IOException,
                                            DisorderParseError,
                                            DisorderProtocolError {
    connect();
    send("edituser %s %s %s", quote(username), quote(property), quote(value));
    getPositiveResponse();
  }

  /**
   * Enable play
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void enable() throws IOException,
                              DisorderParseError,
                              DisorderProtocolError {
    connect();
    send("enable");
    getPositiveResponse();
  }

  /**
   * Test whether play is enabled.
   *
   * @return <code>true</code> if play is enabled, otherwise <code>false</code>
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public boolean enabled() throws IOException,
                                  DisorderParseError,
                                  DisorderProtocolError {
    connect();
    send("enabled");
    return getBooleanResponse();
  }

  /**
   * Test whether a track exists.
   *
   * @param track Track to check
   * @return <code>true</code> if the track exists, otherwise <code>false</code>
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public boolean exists(String track) throws IOException,
                                  DisorderParseError,
                                             DisorderProtocolError {
    connect();
    send("exists %s", quote(track));
    return getBooleanResponse();
  }

  /**
   * Get a list of files.
   *
   * Returns a list of the files immediately below
   * <code>path</code>.  If <code>regexp</code> is not
   * <code>null</code> then only matching names are returned.
   *
   * @param path Parent directory
   * @param regexp Regular expression to filter results, or <code>null</code>
   * @return List of files
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> files(String path,
                              String regexp) throws IOException,
                                                    DisorderParseError,
                                                    DisorderProtocolError {
    connect();
    if(regexp != null)
      send("files %s %s", quote(path), quote(regexp));
    else
      send("files %s", quote(path));
    getPositiveResponse();
    return getResponseBody();
  }

  /**
   * Get a track preference value.
   *
   * @param track Track to look up
   * @param pref Preference name
   * @return Preference value, or <code>null</code> if it's not set
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public String get(String track,
                    String pref) throws IOException,
                                        DisorderParseError,
                                        DisorderProtocolError {
    connect();
    send("get %s %s", quote(track), quote(pref));
    return getStringResponse(true);
  }

  /**
   * Get a global preference value
   *
   * @param key Preference name
   * @return Preference value, or <code>null</code> if it's not set
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public String getGlobal(String key) throws IOException,
                                             DisorderParseError,
                                             DisorderProtocolError {
    connect();
    send("get-global %s %s", quote(key));
    return getStringResponse(true);
  }

  /**
   * Get a track's length.
   *
   * @param track Track to look up
   * @return Track length in seconds
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public int length(String track) throws IOException,
                                         DisorderParseError,
                                         DisorderProtocolError {
    connect();
    send("length %s", quote(track));
    return Integer.parseInt(getStringResponse(false));
  }

  // TODO log not implemented yet
  // TODO make-cookie not implemented because only used by CGI

  /**
   * Move a track.
   *
   * <p>If <code>delta</code> is positive then the track is moved
   * towards the head of the queue.  If it is negative then it is
   * moved towards the tail of the queue.
   *
   * <p>Tracks be identified by ID or name but ID is preferred.
   *
   * @param track Track ID or name
   * @param delta How far to move track
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void move(String track,
                   int delta) throws IOException,
                                         DisorderParseError,
                                         DisorderProtocolError {
    connect();
    send("move %s %d", quote(track), delta);
    getPositiveResponse();
  }

  /**
   * Move multiple tracks.
   *
   * ALl of the listed track IDs will be moved so that they appear
   * just after the target, but retaining their relative ordering
   * within themselves.
   *
   * @param target Target track ID
   * @param tracks Track IDs to move
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void moveafter(String target,
                        String... tracks) throws IOException,
                                                 DisorderParseError,
                                                 DisorderProtocolError {
    connect();
    StringBuilder sb = new StringBuilder();
    for(int n = 0; n < tracks.length; ++n) {
      sb.append(' ');
      sb.append(quote(tracks[n]));
    }
    send("moveafter %s %s", quote(target), sb.toString());
    getPositiveResponse();
  }

  /**
   * Get a list of newly added tracks.
   *
   * If <code>max</code> is positive then at most that many tracks
   * will be sent.  Otherwise the server will choose the maximum to
   * send (and in any case it may impose an upper limit).
   *
   * @param max Maximum number of tracks to get, or 0
   * @return List of new tracks
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> newTracks(int max) throws IOException,
                                                DisorderParseError,
                                                DisorderProtocolError {
    connect();
    if(max > 0)
      send("new %d", max);
    else
      send("new");
    getPositiveResponse();
    return getResponseBody();
  }

  /**
   * Do nothing.
   *
   * <p>Typically used to ensure that a failed network connection is
   * quickly detected.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void nop() throws IOException,
                           DisorderParseError,
                           DisorderProtocolError {
    connect();
    send("nop");
    getPositiveResponse();
  }

  /**
   * Get a track name part.
   *
   * <p><code>context</code> should either <code>"sort"</code> or
   * <code>"display"</code>.  <code>part</code> is generally either
   * <code>"artist"</code>, <code>"album"</code> or <code>"title"</code>.
   *
   * @param track Track to look up
   * @param context Context for the lookup
   * @param part Part of the track name to get
   * @return Track name part or the empty string
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public String part(String track,
                     String context,
                     String part) throws IOException,
                           DisorderParseError,
                           DisorderProtocolError {
    connect();
    send("part %s %s %s", quote(track), quote(context), quote(part));
    return getStringResponse(false);
  }

  /**
   * Pause play.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void pause() throws IOException,
                              DisorderParseError,
                              DisorderProtocolError {
    connect();
    send("pause");
    getPositiveResponse();
  }

  /**
   * Add a track to the queue.
   *
   * @param track Track to queue
   * @return Track ID in queue
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public String play(String track) throws IOException,
                                  DisorderParseError,
                                             DisorderProtocolError {
    connect();
    send("play %s", quote(track));
    return getStringResponse(false);
  }

  /**
   * Play multiple tracks.
   *
   * All of the listed tracks will be insered into the qeueu just
   * after the target, retaining their relative ordering within
   * themselves.
   *
   * <p><b>Note</b>: in current server implementations the underlying
   * command does not return new track IDs.  This may change in the
   * future.  In that case a future implementation of this method will
   * return a list of track IDs.  However in the mean time callers
   * <u>must</u> be able to cope with a <code>null</code> return from
   * this method.
   *
   * @param target Target track ID
   * @param tracks Tracks to add to the queue
   * @return Currently <code>null</code>, but see note above
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> playafter(String target,
                        String... tracks) throws IOException,
                                                 DisorderParseError,
                                                 DisorderProtocolError {
    connect();
    StringBuilder sb = new StringBuilder();
    for(int n = 0; n < tracks.length; ++n) {
      sb.append(' ');
      sb.append(quote(tracks[n]));
    }
    send("playafter %s %s", quote(target), sb.toString());
    getPositiveResponse();
    return null;
  }

  /**
   * Get the playing track.
   *
   * @return Information for the playing track or <code>null</code> if nothing is playing
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public TrackInformation playing() throws IOException,
                                           DisorderParseError,
                                           DisorderProtocolError {
    connect();
    send("playing");
    Vector<String> v = getPositiveResponse();
    if(v.get(0).equals("252"))
      return new TrackInformation(v, 1);
    else
      return null;
  }

  /**
   * Delete a playlist.
   *
   * @param playlist Playlist to delete
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void playlistDelete(String playlist) throws IOException,
                                                     DisorderParseError,
                                                     DisorderProtocolError {
    connect();
    send("playlist-delete %s", quote(playlist));
    getPositiveResponse();
  }

  /**
   * Get the contents of a playlist.
   *
   * @param playlist Playlist to get
   * @return Playlist contents
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> playlistGet(String playlist)
    throws IOException,
           DisorderParseError,
           DisorderProtocolError {
    connect();
    send("playlist-get %s", quote(playlist));
    return getResponseBody();
  }

  /**
   * Get the sharing status of a playlist.
   *
   * Possible sharing statuses are <code>"public"</code>,
   * <code>"private"</code> and <code>"shared"</code>.
   *
   * @param playlist Playlist to get sharing status for
   * @return Sharing status of playlist.
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public String playlistGetShare(String playlist)
    throws IOException,
           DisorderParseError,
           DisorderProtocolError {
    connect();
    send("playlist-get-share %s", quote(playlist));
    return getStringResponse(false);
  }

  /**
   * Lock a playlist.
   *
   * @param playlist Playlist to lock
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void playlistLock(String playlist) throws IOException,
                                                     DisorderParseError,
                                                     DisorderProtocolError {
    connect();
    send("playlist-lock %s", quote(playlist));
    getPositiveResponse();
  }

  /**
   * Set the contents of a playlist.
   *
   * The playlist must be locked.
   *
   * @param playlist Playlist to modify
   * @param contents New contents
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void playlistSet(String playlist,
                          List<String> contents) throws IOException,
                                                        DisorderParseError,
                                                        DisorderProtocolError {
    connect();
    send("playlist-set %s", quote(playlist));
    for(String s: contents)
      send("%s%s", s.charAt(0) == '.' ? "." : "", s);
    send(".");
    // TODO send() will flush after every line which isn't very efficient
    getPositiveResponse();
  }

  /**
   * Set the sharing status of a playlist.
   *
   * The playlist must be locked.
   *
   * Possible sharing statuses are <code>"public"</code>,
   * <code>"private"</code> and <code>"shared"</code>.
   *
   * @param playlist Playlist to delete
   * @param share New sharing status
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void playlistSetShare(String playlist,
                               String share) throws IOException,
                                                    DisorderParseError,
                                                    DisorderProtocolError {
    connect();
    send("playlist-set-share %s %s", quote(playlist), quote(share));
    getPositiveResponse();
  }

  /**
   * Unlock the locked playlist.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public void playlistUnlock(String playlist) throws IOException,
                                                     DisorderParseError,
                                                     DisorderProtocolError {
    connect();
    send("playlist-unlock");
    getPositiveResponse();
  }

  /**
   * Get a list of playlists.
   *
   * Private playlists belonging to other users aren't included in the
   * result.
   *
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public List<String> playlists() throws IOException,
                                         DisorderParseError,
                                         DisorderProtocolError {
    connect();
    send("playlists");
    return getResponseBody();
  }

  // TODO lots more to do...

  /**
   * Get the server version.
   *
   * <p>The version string will generally be in one of the following formats:
   * <ul>
   * <li><code>x.y</code> - a major release
   * <li><code>x.y.z</code> - a minor release
   * <li><code>x.y+</code> - an intermediate development version
   * </ul>
   *
   * @return Server version string
   * @throws IOException If a network IO error occurs
   * @throws DisorderParseError If a malformed response was received
   * @throws DisorderProtocolError If the server sends an error response
   */
  public String version() throws IOException,
                                 DisorderParseError,
                                 DisorderProtocolError {
    connect();
    send("version");
    return getPositiveResponse().get(1);
  }

  
}

/*
Local Variables:
mode:java
indent-tabs-mode:nil
c-basic-offset:2
End:
*/
