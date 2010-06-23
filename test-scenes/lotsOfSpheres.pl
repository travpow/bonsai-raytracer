#!/usr/bin/perl

# Generates scenes with lots of spheres of various sizes.
# For performance testing.
# Number of spheres is a command line parameter.

use strict;
use warnings;

use JSON::XS;

use constant MIN_R => 0.5;
use constant MAX_R => 5;
use constant BOX_SIZE => 200;
use constant MIN_DISTANCE_FROM_CAMERA => 15;
use constant DEFAULT_COUNT => 200;


my $count = pop @ARGV;
$count = DEFAULT_COUNT unless defined($count);

my @objects;

for(my $i = 0; $i < $count; ++$i){
	my $r = rand(MAX_R - MIN_R) + MIN_R;

	my $x;
	my $y;
	my $z;

	do{
		$x = rand(BOX_SIZE) - BOX_SIZE / 2;
		$y = rand(BOX_SIZE) - BOX_SIZE / 2;
		$z = rand(BOX_SIZE) - BOX_SIZE / 2;

	}while($x * $x + $y * $y + $z * $z < MIN_DISTANCE_FROM_CAMERA * MIN_DISTANCE_FROM_CAMERA);

	push @objects, {
		type => 'sphere',
		r => $r,
		center => [$x, $y, $z]
	};
}

print encode_json({
	objects => \@objects,
	title => "$count spheres generated by $0",
	camera => {
		raysPerPx => 10,
	}
}), "\n";