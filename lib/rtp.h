/*
 * This file is part of DisOrder.
 * Copyright (C) 2007 Richard Kettlewell
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

#ifndef RTP_H
#define RTP_H

/* RTP is defined in RFC1889 */
struct attribute((packed)) rtp_header {
  uint8_t vpxcc;
  uint8_t mpt;
  uint16_t seq;
  uint32_t timestamp;
  uint32_t ssrc;
};

#endif /* RTP_H */

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
